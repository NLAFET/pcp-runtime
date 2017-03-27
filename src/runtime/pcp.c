#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/time.h>
#include <hwloc.h>

#include "pcp.h"


////////////////////////////////////////////////////////////////////////////////
// INTERNAL DATA STRUCTURES
////////////////////////////////////////////////////////////////////////////////


#define MAX_NUM_TASK_TYPES 64
#define MAX_NUM_MESSAGES 4
#define MAX_NUM_ADAPTIONS 1024


/**
 * Represents a task in the task graph.
 */
struct task
{
    /** User-defined task instance parameters. */
    void *arg;
    
    /** The tasks at the head end of the outgoing edges. */
    struct task **out;

    /** Handle to the task type. */
    pcp_task_type_handle_t type;

    /** Handle to self. */
    pcp_task_handle_t handle;

    /** The priority of the task. */
    float priority;

    /** Task identification number. */
    int seqnr;

    /** The number of incoming edges. */
    int num_in;

    /** The number of incoming edges whose corresponding source task
        has been executed. */
    int num_in_ready;

    /** The number of outgoing edges. */
    int num_out;

    /** The current capacity of the outgoing array. */
    int out_capacity;

    /** The index of the single outgoing edge that lies on the
        critical path (only meaningful if is_critical == true and
        num_out > 0). */
    int critical_edge;

    /** Set when the task is on the critical path. */
    bool is_critical;

    /** The task has been executed. */
    bool is_executed;

    int prescribed_num_cores;

    double time_start;
    double time_end;
    int first_core;
    int num_cores;

    double time;
    double longest_path;
    double second_longest_path;
};


/**
 * Represents a task graph.
 */
struct graph
{
    /** The tasks. */
    struct task **tasks;

    /** The number of tasks. */
    int num_tasks;

    /** The number of executed tasks. */
    int num_executed_tasks;

    /** The current capacity of the tasks array. */
    int tasks_capacity;
};


/**
 * The subset of the tasks that are ready for execution.
 */
struct queue
{
    /** Array of pointers to tasks. */
    struct task **tasks;
    
    /** The number of tasks. */
    int num_tasks;

    /** The capacity of the tasks array. */
    int tasks_capacity;
};


enum command_type { CMD_EXECUTE, CMD_SHRINK, CMD_GROW };


struct command
{
    /** The type of command. */
    enum command_type type;

    /** The body of the command. */
    union
    {
        /** The body of a command of type CMD_EXECUTE. */
        struct
        {
            /** The ready (parallel) task to execute. */
            struct task *task;
        } execute;

        /** The body of a command of type CMD_SHRINK. */
        struct
        {
            /** The new smaller barrier. */
            spin_barrier_t *barrier;
        } shrink;

        /** The body of a command of type CMD_GROW. */
        struct
        {
            /** The new larger barrier. */
            spin_barrier_t *barrier;
        } grow;
    } body;
};


enum message_type { MSG_EXECUTE, MSG_RECRUIT, MSG_TERMINATE, MSG_WARMUP };


struct message
{
    /** The type of message. */
    enum message_type type;

    /** The body of the message. */
    union
    {
        /** The body of a message of type MSG_EXECUTE. */
        struct
        {
            /** The ready task to execute. */
            struct task *task;
        } execute;

        /** The body of a message of type MSG_RECRUIT. */
        struct
        {
            /** The new larger barrier. */
            spin_barrier_t *barrier;
        } recruit;

        /** The body of a message of type MSG_TERMINATE. */
        struct
        {
        } terminate;

        /** The body of a message of type MSG_WARMUP. */
        struct
        {
        } warmup;
    } body;
};


/**
 * Single thread message queue. 
 */
struct mqueue
{
    /** Array of messages. */
    struct message messages[MAX_NUM_MESSAGES];

    /** Condition variable for synchronization. */
    pthread_cond_t cv_has_message;

    /** The number of messages in the queue. */
    int num_messages;

    /** The index of the head of the queue. */
    int head;

    /** The index of the tail of the queue. */
    int tail;
};


/**
 * Represents per-worker state. 
 */
struct worker
{
    /** The PThread structure. */
    pthread_t thread;

    /** The message queue. */
    struct mqueue mqueue;

    /** The rank of the worker. */
    int rank;

    /** Signals that the worker is idle. */
    bool is_idle;

    /** Execution time of critical tasks. */
    double time_critical_tasks;

    /** Time spent in critical tasks. */
    double cost_critical_tasks;

    /** Time spent in non-critical tasks. */
    double cost_noncritical_tasks;

    /** Time spent being a critical worker. */
    double time_critical_worker;
};


/**
 * Represents the runtime system's internal state.
 */
struct state
{
    /** The registered task types. */
    struct pcp_task_type task_types[MAX_NUM_TASK_TYPES];

    /** The task graph. */
    struct graph graph;

    /** The set of ready tasks. */
    struct queue queue;

    /** The workers' internal states. */
    struct worker *workers;

    /**
     * Lock protecting some of the members of this structure.
     *
     * The following members are protected by this lock:
     *
     * - graph
     * - queue
     * - workers
     * - num_arrived_recruits
     */
    pthread_mutex_t lock;

    /** Condition variable for master waiting on critical task. */
    pthread_cond_t cv_master;

    /** Barrier for synchronization of all threads at startup. */
    spin_barrier_t *barrier;

    /** Low-latency barrier for synchronization between master and
        critical workers. */
    spin_barrier_t *critical_barrier;

    /** Command structure for master -> critical worker communication. */
    struct command command;

    /** The number of workers (including the master). */
    int num_workers;

    /** The current number of critical workers (including the master). */
    int num_critical_workers;

    /** Mode of execution. */
    int mode;

    /** Fixed number of critical workers if mode == PCP_FIXED. */
    int fixed_num_critical_workers;

    /** Flags if there is an ongoing recruitment. */
    bool is_recruiting;

    /** The number of sought recruits. */
    int num_recruits;

    /** The number of arrived recruits. */
    int num_arrived_recruits;

    /** The number of registered task types. */
    int num_task_types;

    /** The execution time as measured by the master. */
    double execution_time;

    /** The adaption event trace. */
    struct pcp_adaption_event adaption_events[MAX_NUM_ADAPTIONS];

    /** The number of adaption events. */
    int num_adaption_events;
};


/** File-scope variable representing the entire runtime system state. */
static struct state state;


/* Variables used to support thread affinity. */
static hwloc_topology_t topology;


////////////////////////////////////////////////////////////////////////////////
// INTERNAL FUNCTIONS
////////////////////////////////////////////////////////////////////////////////


/**
 * Gets time of day in seconds.
 *
 * @return Time of day.
 */
static double gettime(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + 1e-6 * tv.tv_usec;
}


/**
 * Acquires the runtime lock.
 *
 * @note The runtime lock must not be held.
 */
static void lock(void)
{
    pthread_mutex_lock(&state.lock);
}


/**
 * Releases the runtime lock.
 *
 * @note The runtime lock must be held.
 */
static void unlock(void)
{
    pthread_mutex_unlock(&state.lock);
}


/**
 * Maps a task type to a color for TikZ trace output.
 *
 * @param [in] task_type The task type.
 *
 * @param [out] color The color (array of size at least 16 bytes long).
 */
static void task_type_2_color(pcp_task_type_handle_t task_type, char *color)
{
    switch(task_type) {
        case 0:
            strcpy(color, "blue");
            break;
        case 1:
            strcpy(color, "orange");
            break;
        case 2:
            strcpy(color, "red");
            break;
        case 3:
            strcpy(color, "violet");
            break;
        case 4:
            strcpy(color, "forestgreen");
            break;
        case 5:
            strcpy(color, "cyan");
            break;
        case 6:
            strcpy(color, "gold");
            break;
        case 7:
            strcpy(color, "navy");
            break;
        default:
            // We are out of colors.
            strcpy(color, "black");
            break;
    }
}


////////////////////////////////////////////////////////////////////////////////
// Core frequency functions
//

static int nominal_clock_frequency = 0;


static void estimate_nominal_frequency(void)
{
    FILE *fp;
    int freq = 0;
    fp = popen("dmesg | grep \"tsc: Detected\"", "r");
    if (fp != NULL) {
        char line[128];
        fgets(line, sizeof(line), fp);
        char *ptr = strstr(line, "tsc: Detected");
        if (ptr != NULL) {
            ptr += 14;
            freq = 1000 * atol(ptr);
        }
    }
    if (freq != 0) {
        printf("[INFO] Determined the nominal clock frequency to %d KHz\n", freq);
    } else {
        printf("[WARNING] Failed to estimate the nominal clock frequency (warmup disabled)\n");
    }
    nominal_clock_frequency = freq;
}


static int get_frequency(int core)
{
    static char *paths[] =
        {
            "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu1/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu2/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu3/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu5/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu6/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu8/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu9/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu10/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu11/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu12/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu13/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu14/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu15/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu16/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu17/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu18/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu19/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu20/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu21/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu22/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu23/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu24/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu25/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu26/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu27/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu28/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu29/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu30/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu31/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu32/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu33/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu34/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu35/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu36/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu37/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu38/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu39/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu40/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu41/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu42/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu43/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu44/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu45/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu46/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu47/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu48/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu49/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu50/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu51/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu52/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu53/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu54/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu55/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu56/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu57/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu58/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu59/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu60/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu61/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu62/cpufreq/scaling_cur_freq",
            "/sys/devices/system/cpu/cpu63/cpufreq/scaling_cur_freq"
        };
    if (core < 0 || core >= 64) {
        return 0;
    }
    FILE *fp = fopen(paths[core], "r");
    if (fp != NULL) {
        int freq;
        fscanf(fp, "%d", &freq);
        fclose(fp);
        return freq;
    }
    return 0;
}


static void warmup(int core)
{
    // Synchronize.
    spin_barrier_wait(state.barrier);

    // Warm up.
    while (get_frequency(core) < 0.99 * nominal_clock_frequency) {
        // empty
    }
    
    // Synchronize.
    spin_barrier_wait(state.barrier);
}


////////////////////////////////////////////////////////////////////////////////
// Thread affinity functions
//

static int get_num_cores(void)
{
    int cnt = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_CORE);
    return cnt;
}


static void thread_affinity_init(void)
{
    hwloc_topology_init(&topology);
    hwloc_topology_load(topology);
    printf("[INFO] Found %d cores\n", get_num_cores());
}


static void thread_affinity_finalize(void)
{
    hwloc_topology_destroy(topology);
}


static void bind_thread_to_core(int core)
{
    int num_cores = get_num_cores();
    hwloc_obj_t obj;
    obj = hwloc_get_obj_by_type(topology, HWLOC_OBJ_CORE, core);
    hwloc_set_cpubind(topology, obj->cpuset, HWLOC_CPUBIND_THREAD);
}


////////////////////////////////////////////////////////////////////////////////
// Adaption trace functions.
//


/**
 * Add adaption event at this time.
 *
 * @param count The new critical worker count.
 */
static void add_adaption_event(int count)
{
    if (state.num_adaption_events < MAX_NUM_ADAPTIONS) {
        int pos = state.num_adaption_events;
        state.num_adaption_events += 1;
        state.adaption_events[pos].time = gettime();
        state.adaption_events[pos].count = count;
    }
}


////////////////////////////////////////////////////////////////////////////////
// Command functions.
//


/**
 * Waits for a command.
 *
 * @param barrier The current barrier.
 *
 * @return The received command.
 */
static struct command cmd_wait(spin_barrier_t *barrier)
{
    // Wait for the command.
    spin_barrier_wait(barrier);
    
    // Return the received command.
    return state.command;
}


/**
 * Acknowledges completed processing of a command.
 *
 * @param barrier The current barrier.
 */
static void cmd_ack(spin_barrier_t *barrier)
{
    spin_barrier_wait(barrier);
}


/**
 * Broadcasts a CMD_EXECUTE command.
 *
 * @param barrier The barrier.
 *
 * @param task The ready task to execute.
 */
static void cmd_bcast_execute(spin_barrier_t *barrier, struct task *task)
{
    // Populate the command structure.
    state.command.type = CMD_EXECUTE;
    state.command.body.execute.task = task;
    
    // Signal the workers.
    spin_barrier_wait(barrier);
}


/**
 * Broadcasts a CMD_SHRINK command.
 *
 * @param barrier The current barrier.
 *
 * @param small_barrier The new smaller barrier.
 */
static void cmd_bcast_shrink(spin_barrier_t *barrier, spin_barrier_t *small_barrier)
{
    // Populate the command structure.
    state.command.type = CMD_SHRINK;
    state.command.body.shrink.barrier = small_barrier;
    
    // Signal the workers.
    spin_barrier_wait(barrier);
}


/**
 * Broadcasts a CMD_GROW command.
 *
 * @param barrier The current barrier.
 *
 * @param large_barrier The new larger barrier.
 */
static void cmd_bcast_grow(spin_barrier_t *barrier, spin_barrier_t *large_barrier)
{
    // Populate the command structure.
    state.command.type = CMD_GROW;
    state.command.body.grow.barrier = large_barrier;
    
    // Signal the workers.
    spin_barrier_wait(barrier);
}


/**
 * Wait for the workers to acknowledge processing of command.
 *
 * @param barrier The barrier.
 */
static void cmd_bcast_wait(spin_barrier_t *barrier)
{
    spin_barrier_wait(barrier);
}


//////////////////////////////////////////////////////////////////////////////// 
// Message queue functions.
// 


static void mqueue_init(int rank);
static void mqueue_destroy(int rank);
static struct message mqueue_wait(int rank);
static struct message *mqueue_allocate(int rank);
static void mqueue_signal(int rank);
static void mqueue_send_execute(int rank, struct task *task);
static void mqueue_send_recruit(int rank, spin_barrier_t *barrier);
static void mqueue_send_terminate(int rank);


/**
 * Initializes a message queue.
 *
 * @param rank The worker's rank.
 */
static void mqueue_init(int rank)
{
    struct mqueue *mq = &state.workers[rank].mqueue;
    pthread_cond_init(&mq->cv_has_message, NULL);
    mq->num_messages = 0;
    mq->head = mq->tail = 0;
}


/**
 * Destroys a message queue.
 *
 * @param rank The worker's rank.
 *
 * @note The queue must be empty.
 */
static void mqueue_destroy(int rank)
{
    struct mqueue *mq = &state.workers[rank].mqueue;
    pthread_cond_destroy(&mq->cv_has_message);
}


/**
 * Waits until a message arrives and then returns it.
 *
 * @param [in] rank The rank of the calling thread.
 *
 * @return The received message.
 *
 * @note The runtime lock must be held.
 */
static struct message mqueue_wait(int rank)
{
    // Locate the message queue.
    struct mqueue *mq = &state.workers[rank].mqueue;

    // Loop until there is some message in the queue.
    while (mq->num_messages == 0) {
        // Mark as idle.
        state.workers[rank].is_idle = true;
        
        // Suspend the thread until signalled. 
        pthread_cond_wait(&mq->cv_has_message, &state.lock);
    }

    // Pop from the head of the queue.
    int pos = mq->head;
    mq->head = (mq->head + 1) % MAX_NUM_MESSAGES;
    mq->num_messages -= 1;
    struct message msg = mq->messages[pos];

    // Return the message. 
    return msg;
}


/**
 * Allocates a message on a workers's message queue.
 *
 * @param [in] rank The worker's rank.
 *
 * @note The runtime lock must be held.
 */
static struct message *mqueue_allocate(int rank)
{
    // Locate the message queue.
    struct mqueue *mq = &state.workers[rank].mqueue;

    // Append to the tail.
    int pos = mq->tail;
    mq->tail = (mq->tail + 1) % MAX_NUM_MESSAGES;
    mq->num_messages += 1;

    // Return allocated message structure.
    return mq->messages + pos;
}


/**
 * Signals a message queue's condition variable.
 *
 * @param [in] rank The worker's rank.
 *
 * @note The runtime lock must be held.
 */
static void mqueue_signal(int rank)
{
    // Mark as not idle.
    state.workers[rank].is_idle = false;
    
    // Signal the condition variable.
    pthread_cond_signal(&state.workers[rank].mqueue.cv_has_message);
}


/**
 * Sends an MSG_EXECUTE message to specified thread rank.
 *
 * @param [in] rank Message destination.
 *
 * @param [in] task The ready task to execute.
 *
 * @note The runtime lock must be held. 
 */
static void mqueue_send_execute(int rank, struct task *task)
{
    // Allocate a message structure on the queue.
    struct message *msg = mqueue_allocate(rank);

    // Populate the message structure.
    msg->type = MSG_EXECUTE;
    msg->body.execute.task = task;

    // Signal the worker.
    mqueue_signal(rank);
}


/**
 * Sends an MSG_RECRUIT message to specified thread rank.
 *
 * @param [in] rank Message destination.
 *
 * @param [in] barrier The new larger barrier.
 *
 * @note The runtime lock must be held.
 */
static void mqueue_send_recruit(int rank, spin_barrier_t *barrier)
{
    // Allocate a message structure on the queue.
    struct message *msg = mqueue_allocate(rank);

    // Populate the message structure.
    msg->type = MSG_RECRUIT;
    msg->body.recruit.barrier = barrier;

    // Signal the worker.
    mqueue_signal(rank);
}


/**
 * Sends an MSG_TERMINATE message to specified thread rank.
 *
 * @param [in] rank Message destination.
 *
 * @note The runtime lock must be held.
 */
static void mqueue_send_terminate(int rank)
{
    // Allocate a message structure on the queue.
    struct message *msg = mqueue_allocate(rank);

    // Populate the message structure.
    msg->type = MSG_TERMINATE;

    // Signal the worker.
    mqueue_signal(rank);
}


/**
 * Sends an MSG_WARMUP message to specified thread rank.
 *
 * @param [in] rank Message destination.
 *
 * @note The runtime lock must be held.
 */
static void mqueue_send_warmup(int rank)
{
    // Allocate a message structure on the queue.
    struct message *msg = mqueue_allocate(rank);

    // Populate the message structure.
    msg->type = MSG_WARMUP;

    // Signal the worker.
    mqueue_signal(rank);
}


////////////////////////////////////////////////////////////////////////////////
// Functions for managing the shared queue of ready tasks.
//


/**
 * Initialize the shared queue.
 */
static void queue_init(void)
{
    state.queue.tasks_capacity = 1000;
    state.queue.tasks = (struct task**) malloc(sizeof(struct task*) * state.queue.tasks_capacity);
    state.queue.num_tasks = 0;
}


/**
 * Destroys the shared queue.
 */
static void queue_destroy(void)
{
    free(state.queue.tasks);
    state.queue.tasks = NULL;
}


/**
 * Pushes a ready task to the shared queue.
 *
 * @param task The ready task. 
 */
static void queue_push(struct task *task)
{
    // Select location of the task in state.queue.tasks. 
    int pos = state.queue.num_tasks;

    // Bump the task counter.
    state.queue.num_tasks += 1;

    // Will we overflow the capacity of the array?
    if (state.queue.num_tasks > state.queue.tasks_capacity) {
        // Double the array capacity.
        state.queue.tasks_capacity *= 2;
        state.queue.tasks = realloc(state.queue.tasks, sizeof(struct task*) * state.queue.tasks_capacity);
    }

    // Add task to the array.
    state.queue.tasks[pos] = task;
}


/**
 * Pops the task with the highest priority from the shared queue.
 *
 * @return The ready task with the highest priority or NULL if empty.
 */
static struct task *queue_pop(void)
{
    // Quick return if possible.
    if (state.queue.num_tasks == 0) {
        return NULL;
    }

    // The position of the highest priority task found so far.
    int pos = 0;

    // The priority of the best task.
    float best = state.queue.tasks[pos]->priority;

    // Loop through all remaining ready tasks.
    for (int i = 1; i < state.queue.num_tasks; ++i) {
        // Extract its priority.
        float prio = state.queue.tasks[i]->priority;

        // Better than the best found so far?
        if (prio > best) {
            // Update the best.
            best = prio;
            pos = i;
        }
    }

    // Extract the highest priority task.
    struct task *task = state.queue.tasks[pos];

    // Move the last task into the hole.
    state.queue.tasks[pos] = state.queue.tasks[state.queue.num_tasks - 1];

    // Decrement the task counter.
    state.queue.num_tasks -= 1;

    // Return the highest priority task. 
    return task;
}


////////////////////////////////////////////////////////////////////////////////
// Task functions.
// 


/**
 * Translate task handle to task pointer.
 *
 * @param [in] handle The task handle.
 *
 * @return The task pointer.
 */
static struct task *handle2task(pcp_task_handle_t handle)
{
    return state.graph.tasks[(int) handle];
}


/**
 * Creates a task.
 *
 * @param type Handle to the task type.
 *
 * @param arg The user-defined task instance parameter.
 *
 * @return A new task on the heap.
 */
static struct task *task_create(pcp_task_type_handle_t type, void *arg)
{
    struct task *task = (struct task*) malloc(sizeof(struct task));
    task->type = type;          // Constant.
    task->arg = arg;            // Constant.
    task->seqnr = state.graph.num_tasks; // Constant.
    task->num_in = 0;           // To be modified when building the graph.
    task->num_out = 0;          // To be modified when building the graph.
    task->out_capacity = 16;    // To be modified when building the graph.
    task->priority = 0;         // To be modified after buildnig the graph.
    task->critical_edge = -1;   // To be modified after building the graph.
    task->is_critical = false;  // To be modified after building the graph.
    task->is_executed = false;  // To be modified when executing the graph.
    task->num_in_ready = 0;     // To be modified when executing the graph.
    task->prescribed_num_cores = -1; // To be modified when executing the graph if mode == PCP_PRESCRIBED.
    task->out = (struct task**) malloc(sizeof(struct task*) * task->out_capacity);
    return task;
}


/**
 * Destroys a task structure.
 *
 * @param task The task structure.
 */
static void task_destroy(struct task *task)
{
    free(task->arg);
    free(task->out);
    free(task);
}


/**
 * Adds an outgoing edge to a task.
 *
 * @param task The source task.
 *
 * @param dest The destination task.
 */
static void task_add_outgoing_edge(struct task *task, struct task *dest)
{
    // Select the position of dest in task->out.
    int pos = task->num_out;

    // Bump the edge counter.
    task->num_out += 1;

    // Will we overflow the capacity of the array?
    if (task->num_out > task->out_capacity) {
        // Double the array capacity.
        task->out_capacity *= 2;
        task->out = realloc(task->out, sizeof(struct task*) * task->out_capacity);
    }

    // Add dest to the array.
    task->out[pos] = dest;
}


/**
 * Adds an incoming edge to a task.
 *
 * @param [in,out] task The destination task.
 *
 * @param [in] src The source task.
 */
static void task_add_incoming_edge(struct task *task, struct task *src)
{
    task->num_in += 1;
}


static void task_distribute_ready(void)
{
    // Send tasks to all idle workers.
    for (int rank = state.num_critical_workers; rank < state.num_workers && state.queue.num_tasks > 0; ++rank) {
        if (state.workers[rank].is_idle) {
            struct task *ready = queue_pop();
            mqueue_send_execute(rank, ready);
        }
    }
}


/**
 * Post-process the task.
 *
 * @param task The task to post-process.
 */
static void task_post_process(struct task *task)
{
    // Record the task as executed.
    task->is_executed = true;
    state.graph.num_executed_tasks += 1;
    if (state.graph.num_executed_tasks == state.graph.num_tasks) {
        // Signal master that all tasks have been executed.
        if (state.num_critical_workers == 0) {
            mqueue_send_terminate(0);
        } else {
            pthread_cond_signal(&state.cv_master);
        }
    }

    // Loop through all outgoing edges.
    for (int i = 0; i < task->num_out; ++i) {
        // Locate the task at the other end.
        struct task *succ = task->out[i];

        // Increment its number of ready incoming edges.
        succ->num_in_ready += 1;

        // Did the successor become ready as a result?
        if (succ->num_in_ready == succ->num_in) {
            // Is the task critical?
            if (succ->is_critical && state.num_critical_workers > 0) {
                // Signal master that a critical task is ready.
                pthread_cond_signal(&state.cv_master);
            } else {
                // Push the task to the ready set.
                queue_push(succ);
            }
        }
    }

    // Send tasks to workers.
    task_distribute_ready();
}


static void task_execute_seq(struct task *task, int rank)
{
    task->time_start = gettime();
    task->first_core = rank;
    task->num_cores = 1;

    struct pcp_task_type *type = state.task_types + task->type;
    type->sequential_impl(task->arg);

    task->time_end = gettime();
    task->time = task->time_end - task->time_start;
}


static void task_execute_par(struct task *task, int rank)
{
    if (rank == 0) {
        // Record start time.
        task->time_start = gettime();
        task->first_core = rank;
        task->num_cores = state.num_critical_workers;

        // Broadcast an CMD_EXECUTE command.
        cmd_bcast_execute(state.critical_barrier, task);
    }

    struct pcp_task_type *type = state.task_types + task->type;
    type->parallel_impl(task->arg, state.num_critical_workers, rank);

    if (rank == 0) {
        // Wait for the task to complete.
        cmd_bcast_wait(state.critical_barrier);

        // Record end time.
        task->time_end = gettime();
        task->time = task->time_end - task->time_start;
    }
}


////////////////////////////////////////////////////////////////////////////////
// Graph functions.
//


static void graph_init(void)
{
    state.graph.tasks_capacity = 1024;
    state.graph.tasks = (struct task**) malloc(sizeof(struct task*) * state.graph.tasks_capacity);
    state.graph.num_tasks = 0;
    state.graph.num_executed_tasks = 0;
}


static void graph_clear(void)
{
    for (int i = 0; i < state.graph.num_tasks; ++i) {
        task_destroy(state.graph.tasks[i]);
    }
    state.graph.num_tasks = 0;
    state.graph.num_executed_tasks = 0;
}


static void graph_destroy(void)
{
    for (int i = 0; i < state.graph.num_tasks; ++i) {
        task_destroy(state.graph.tasks[i]);
    }
    free(state.graph.tasks);
    state.graph.tasks = NULL;
}


static void graph_identify_ready_tasks(void)
{
    for (int i = 0; i < state.graph.num_tasks; ++i) {
        struct task *task = state.graph.tasks[i];

        // Skip critical tasks.
        if (task->is_critical) {
            continue;
        }

        // Ready?
        if (task->num_in == 0) {
            queue_push(task);
        }
    }
}


static double graph_identify_longest_path_aposteriori(void)
{
    const int n = state.graph.num_tasks;
    struct task **tasks = state.graph.tasks;

    // Initialize task variables.
    for (int i = 0; i < n; ++i) {
        tasks[i]->longest_path = tasks[i]->time;
        tasks[i]->second_longest_path = tasks[i]->time;
    }

    // Compute longest and second longest paths.
    for (int i = n - 1; i >= 0; --i) {
        // Find the two largest values of longest_path among the children.
        double first = 0;
        double second = 0;
        for (int j = 0; j < tasks[i]->num_out; ++j) {
            struct task *t = tasks[i]->out[j];
            const double val = t->longest_path;
            if (val > first) {
                first = val;
            } else if (val > second) {
                second = val;
            }
        }

        // Set the longest and second longest paths for task i.
        const double time = tasks[i]->time;
        tasks[i]->longest_path = time + first;
        tasks[i]->second_longest_path = time + second;
    }

    // Find the longest and second longest path.
    double first = 0;
    double second = 0;
    for (int i = 0; i < n; ++i) {
        const double val1 = tasks[i]->longest_path;
        const double val2 = tasks[i]->second_longest_path;
        if (val1 > first) {
            first = val1;
            if (val2 > second) {
                second = val2;
            }
        } else if (val1 > second) {
            second = val1;
        }
    }

    return first;
}                                                     


static void graph_identify_critical_path(void)
{
    // Find task with maximum task priority = head of critical path.
    float maxprio = 0;
    struct task *root = NULL;
    for (int i = 0; i < state.graph.num_tasks; ++i) {
        struct task *task = state.graph.tasks[i];
        if (task->priority > maxprio) {
            maxprio = task->priority;
            root = task;
        }
    }

    // Traverse the critical path.
    for (;;) {
        root->is_critical = true;
        float target = root->priority - 1;
        if (root->num_out == 0) {
            break;
        }
        for (int i = 0; i < root->num_out; ++i) {
            struct task *task = root->out[i];
            if (task->priority == target) {
                root->critical_edge = i;
                root = task;
                break;
            }
        }
    }
}


static float graph_compute_task_priorities_recursively(struct task *root)
{
    if (root->priority == 0) {
        // Find maximum priority of successors.
        float maxprio = 0;
        for (int i = 0; i < root->num_out; ++i) {
            struct task *task = root->out[i];
            float prio = graph_compute_task_priorities_recursively(task);
            if (prio > maxprio) {
                maxprio = prio;
            }
        }

        // Set root priority.
        root->priority = 1 + maxprio;
    }

    // Return root priority.
    return root->priority;
}


static void graph_compute_task_priorities(void)
{
    for (int i = 0; i < state.graph.num_tasks; ++i) {
        struct task *task = state.graph.tasks[i];
        if (task->num_in == 0) {
            graph_compute_task_priorities_recursively(task);
        }
    }
}


////////////////////////////////////////////////////////////////////////////////
// Master functions.
//


static void master_warmup(void)
{
    lock();
    for (int i = 1; i < state.num_workers; ++i) {
        mqueue_send_warmup(i);
    }
    unlock();
    warmup(0);
}


/**
 * Reconfigures all the task types (where applicable).
 *
 * Call this function after changing state.num_critical_workers.
 */
static void reconfigure_task_types(void)
{
    for (int i = 0; i < state.num_task_types; ++i) {
        struct pcp_task_type *type = state.task_types + i;
        if (type->parallel_reconfigure) {
            type->parallel_reconfigure(state.num_critical_workers);
        }
    }
}


/**
 * Start recruitment process.
 *
 * @param size The new (larger) number of critical workers.
 *
 * @return The new larger barrier.
 */
static spin_barrier_t *master_grow_start(int size)
{
    spin_barrier_t *next_barrier;
    
    // Add to trace.
    add_adaption_event(size);

    // Store information about recruitment process.
    state.is_recruiting = true;
    state.num_recruits = size - state.num_critical_workers;
    state.num_arrived_recruits = 0;
            
    // Create new larger barrier.
    next_barrier = spin_barrier_create(state.num_critical_workers + state.num_recruits);

    // Send MSG_RECRUIT message to all recruits.
    for (int i = state.num_critical_workers; i < state.num_critical_workers + state.num_recruits; ++i) {
        mqueue_send_recruit(i, next_barrier);
    }

    return next_barrier;
}


/**
 * Finish recruitment process.
 *
 * @param next_barrier The new larger barrier.
 */
static void master_grow_finish(spin_barrier_t *next_barrier)
{
    // Broadcast CMD_GROW command.
    cmd_bcast_grow(state.critical_barrier, next_barrier);

    // Wait for acknowledgment.
    cmd_bcast_wait(state.critical_barrier);

    // Safely destroy the old barrier.
    spin_barrier_wait_and_destroy(state.critical_barrier);

    // Replace the barrier.
    state.critical_barrier = next_barrier;

    // Finish the recruitment process.
    state.is_recruiting = false;
    state.num_critical_workers += state.num_recruits;
    state.num_recruits = 0;
    state.num_arrived_recruits = 0;

    // Reconfigure the task types.
    reconfigure_task_types();
}


/**
 * Finish recruitment process if possible.
 *
 * @param next_barrier The new larger barrier.
 */
static void master_try_grow_finish(spin_barrier_t *next_barrier)
{
    if (state.is_recruiting && state.num_arrived_recruits == state.num_recruits) {
        master_grow_finish(next_barrier);
    }
}


/**
 * Shrink the number of critical workers.
 *
 * @param size The new number of critical workers.
 */
static void master_shrink(int size)
{
    // Quick return if possible.
    if (state.num_critical_workers == size) {
        return;
    }

    // Add adaption event.
    add_adaption_event(size);

    // Update the number of critical workers.
    state.num_critical_workers = size;

    // Create a smaller barrier.
    spin_barrier_t *small_barrier = spin_barrier_create(size);

    // Broadcast the CMD_SHRINK command.
    cmd_bcast_shrink(state.critical_barrier, small_barrier);

    // Wait for workers to process command.
    cmd_bcast_wait(state.critical_barrier);

    // Safely destroy the old barrier.
    spin_barrier_wait_and_destroy(state.critical_barrier);

    // Replace the barrier with the smaller one.
    state.critical_barrier = small_barrier;

    // Reconfigure the task types.
    reconfigure_task_types();
}


/**
 * Send the MSG_TERMINATE message to all workers.
 */
static void master_terminate(void)
{
    lock();
    for (int i = 1; i < state.num_workers; ++i) {
        mqueue_send_terminate(i);
    }
    unlock();
}


static void master_execute_graph_like_slave(void)
{
    struct worker *me = &state.workers[0];
    bool terminated = false;

    double execution_time = gettime();
    
    // Locate the ready critical task.
    struct task *task = NULL;
    for (int i = 0; i < state.graph.num_tasks; ++i) {
        if (state.graph.tasks[i]->is_critical && state.graph.tasks[i]->num_in == 0) {
            task = state.graph.tasks[i];
            break;
        }
    }

    // Distribute the ready task.
    queue_push(task);
    task_distribute_ready();

    // Loop until terminated.
    while (!terminated) {
        // Acquire the lock.
        lock();
        
        // Wait for a message.
        struct message msg = mqueue_wait(me->rank);

        // Release the lock.
        unlock();

        // Interpret the message.
        switch (msg.type) {
        case MSG_EXECUTE:
        {
            ////////////////////////////////////////////////////////////
            // Execute the task.
            //////////////////////////////////////////////////////////// 

            // Get the task.
            struct task *task = msg.body.execute.task;
            
            // Sequentially execute msg->body.execute.task.
            task_execute_seq(task, me->rank);

            // Accumulate coset of exeucting tasks.
            me->cost_noncritical_tasks += task->time_end - task->time_start;
            
            ////////////////////////////////////////////////////////////
            // Post-process the task.
            //////////////////////////////////////////////////////////// 

            // Acquire the lock.
            lock();

            // Mark as idle if no messages.
            if (me->mqueue.num_messages == 0) {
                me->is_idle = true;
            }
        
            // Post-process the task.
            task_post_process(task);

            // Release the lock.
            unlock();
        }
        break;

        case MSG_TERMINATE:
        {
            terminated = true;
        }
        break;

        default:
            break;
        }
    }

    // Compute and save total execution time.
    execution_time = gettime() - execution_time;
    state.execution_time = execution_time;    
}


/**
 * The master's algorithm for executing a task graph.
 */
static void master_execute_graph(void)
{
    if (state.num_critical_workers == 0) {
        master_execute_graph_like_slave();
        return;
    }
    
    spin_barrier_t *next_barrier = NULL;
    
    // Locate the ready critical task.
    struct task *task = NULL;
    for (int i = 0; i < state.graph.num_tasks; ++i) {
        if (state.graph.tasks[i]->is_critical && state.graph.tasks[i]->num_in == 0) {
            task = state.graph.tasks[i];
            break;
        }
    }
    struct task *first_critical_task = task;

    // Read in prescribed thread counts.
    if (state.mode == PCP_PRESCRIBED) {
        // Count the number of critical tasks.
        int num_critical_tasks = 1;
        for (struct task *t = task; t->num_out > 0; t = t->out[t->critical_edge]) {
            num_critical_tasks += 1;
        }

        // Read in prescribed thread counts.
        printf("[INFO] Reading prescribed thread counts for %d critical tasks from stdin...\n", num_critical_tasks);
        int cnt = 1;
        struct task *t = task;        
        for (int i = 0; i < num_critical_tasks; ++i) {
            if (cnt > 0) {
                scanf("%d", &cnt);
            }
            int real_cnt = cnt > 0 ? cnt : 1;
            t->prescribed_num_cores = real_cnt;
            if (t->num_out > 0) {
                t = t->out[t->critical_edge];
            }
        }
    }
    
    double execution_time = gettime();
    
    // Loop through the critical path one task per iteration.
    while (task != NULL) {
        // Acquire the lock.
        lock();

        // Run control logic for mode PCP_PRESCRIBED.
        if (state.mode == PCP_PRESCRIBED && state.is_recruiting == false) {
            if (task->prescribed_num_cores > state.num_critical_workers) {
                next_barrier = master_grow_start(task->prescribed_num_cores);
            } else if (task->prescribed_num_cores < state.num_critical_workers) {
                master_shrink(task->prescribed_num_cores);
            }
        }

        ////////////////////////////////////////////////////////////
        // Wait for the task to become ready.
        ////////////////////////////////////////////////////////////

        // Loop until ready.
        if (task->num_in_ready != task->num_in) {
            while (task->num_in_ready != task->num_in) {
                pthread_cond_wait(&state.cv_master, &state.lock);
            }
        }

        // Run control logic for mode PCP_PRESCRIBED.
        if (state.mode == PCP_PRESCRIBED) {
            master_try_grow_finish(next_barrier);
        }

        if (state.mode == PCP_ADAPTIVE) {
            ////////////////////////////////////////////////////////////
            // Finalize recruitment process, if any.
            ////////////////////////////////////////////////////////////
            
            master_try_grow_finish(next_barrier);
        }
            
        // Release the lock.
        unlock();
        
        ////////////////////////////////////////////////////////////
        // Execute the task.
        ////////////////////////////////////////////////////////////

        // Get the task implementations.
        struct pcp_task_type *task_type = state.task_types + (int) task->type;

        // Determine how to execute the task (sequentially or in parallel).
        bool sequentially = true;
        if (task_type->parallel_impl == NULL || state.num_critical_workers == 1) {
            sequentially = true;
        } else {
            sequentially = false;
        }

        // Execute the task.
        if (sequentially) {
            // Execute the task sequentially.
            task_execute_seq(task, 0);
        } else {
            // Participate in the parallel execution of the task.
            task_execute_par(task, 0);
        }
        state.workers[0].cost_critical_tasks += task->time * (sequentially ? 1 : state.num_critical_workers);
        state.workers[0].time_critical_tasks += task->time;

        ////////////////////////////////////////////////////////////
        // Post-process the task.
        ////////////////////////////////////////////////////////////

        // Acquire the lock.
        lock();
        
        // Post-process the task.
        task_post_process(task);

        // Release the lock.
        unlock();

        ////////////////////////////////////////////////////////////
        // Advance to the next critical task.
        ////////////////////////////////////////////////////////////
        
        // Advance to the next critical task.
        if (task->num_out > 0) {
            task = task->out[task->critical_edge];
        } else {
            task = NULL;
        }
    }

    // Finish recruitment process.
    if (state.is_recruiting) {
        master_grow_finish(next_barrier);
    }

    // Release all critical workers.
    if (state.num_critical_workers > 1) { 
        master_shrink(1);
    }

    // Wait for completion.
    lock();
    while (state.graph.num_executed_tasks < state.graph.num_tasks) {
        pthread_cond_wait(&state.cv_master, &state.lock);
    }
    unlock();

    // Compute and save total execution time.
    execution_time = gettime() - execution_time;
    state.execution_time = execution_time;
    state.workers[0].time_critical_worker += execution_time;
    
    // Print out actual and prescribed thread counts.
    if (state.mode == PCP_PRESCRIBED) {
        printf("[INFO] Prescribed vs actual thread counts...\n");
        struct task *t = first_critical_task;
        while (t != NULL) {
            if (t->prescribed_num_cores != t->num_cores) {
                printf("[INFO] %3d %3d %+3d\n", t->prescribed_num_cores, t->num_cores, t->num_cores - t->prescribed_num_cores);
            } else {
                printf("[INFO] %3d %3d %3d\n", t->prescribed_num_cores, t->num_cores, 0);
            }
            if (t->num_out > 0) {
                t = t->out[t->critical_edge];
            } else {
                t = NULL;
            }
        }
    }
}


/**
 * Implements the critical worker algorithm.
 *
 * @param me The worker state.
 *
 * @param barrier The barrier for master/worker synchronization.
 */
static void critical_worker(struct worker *me, spin_barrier_t *barrier)
{
    double tm = gettime();
    
    for (;;) {
        // Wait for a command.
        struct command cmd = cmd_wait(barrier);

        // Interpret the command.
        switch (cmd.type) {
        case CMD_EXECUTE:
        {
            // Get the task.
            struct task *task = cmd.body.execute.task;

            // Participate in the parallel execution of cmd.body.execute.task.
            task_execute_par(task, me->rank);

            // Signal completion of task.
            cmd_ack(barrier);
        }
        break;

        case CMD_SHRINK:
        {
            // Signal reception of command.
            cmd_ack(barrier);

            // Safely destroy the old barrier.
            spin_barrier_wait_and_destroy(barrier);

            // Am I one of the critical workers?
            if (me->rank < state.num_critical_workers) {
                // Yes.

                // Use the new smaller barrier.
                barrier = cmd.body.shrink.barrier;
            } else {
                // No.

                // Account for time as critical worker.
                tm = gettime() - tm;
                me->time_critical_worker += tm;

                // Return back to the non-critical worker algorithm.
                return;
            }
        }
        break;

        case CMD_GROW:
        {
            // Signal reception of command.
            cmd_ack(barrier);

            // Safely destroy the old barrier.
            spin_barrier_wait_and_destroy(barrier);
            
            // Use the new larger barrier.
            barrier = cmd.body.grow.barrier;
        }
        break;
        }
    }
}


/**
 * Implements the non-critical worker algorithm.
 *
 * @param ptr The worker state of type struct worker. 
 */
static void *worker_thread(void *ptr)
{
    struct worker *me = (struct worker*) ptr;

    // Set thread affinity.
    bind_thread_to_core(me->rank);

    bool terminated = false;

    // Loop until terminated.
    while (!terminated) {
        // Acquire the lock.
        lock();
        
        // Wait for a message.
        struct message msg = mqueue_wait(me->rank);

        // Release the lock.
        unlock();

        // Interpret the message.
        switch (msg.type) {
        case MSG_EXECUTE:
        {
            ////////////////////////////////////////////////////////////
            // Execute the task.
            //////////////////////////////////////////////////////////// 

            // Get the task.
            struct task *task = msg.body.execute.task;

            // Sequentially execute msg->body.execute.task.
            task_execute_seq(task, me->rank);

            // Accumulate coset of exeucting tasks.
            me->cost_noncritical_tasks += task->time_end - task->time_start;
            
            ////////////////////////////////////////////////////////////
            // Post-process the task.
            //////////////////////////////////////////////////////////// 

            // Acquire the lock.
            lock();

            // Mark as idle if no messages.
            if (me->mqueue.num_messages == 0) {
                me->is_idle = true;
            }
        
            // Post-process the task.
            task_post_process(task);

            // Release the lock.
            unlock();
        }
        break;

        case MSG_RECRUIT:
        {
            // Notify master of arrival.
            lock();
            state.num_arrived_recruits += 1;
            unlock();

            // Run the critical worker algorithm and then continue.
            critical_worker(me, msg.body.recruit.barrier);

            // Check if there is a task for me.
            lock();
            if (me->mqueue.num_messages == 0 && state.queue.num_tasks > 0) {
                mqueue_send_execute(me->rank, queue_pop());
            }
            unlock();
        }
        break;

        case MSG_TERMINATE:
            // Terminate. 
            terminated = true;
            break;

        case MSG_WARMUP:
            warmup(me->rank);
            break;
        }
    }

    return NULL;
}


////////////////////////////////////////////////////////////////////////////////
// API FUNCTIONS
////////////////////////////////////////////////////////////////////////////////


double pcp_get_time(void)
{
    return gettime();
}


void pcp_start(int num_workers)
{
    printf("[INFO] Starting\n");
    
    // Estimate nominal core frequency.
    estimate_nominal_frequency();

    // Initialize thread affinity.
    thread_affinity_init();

    // Get the number of cores.
    int num_cores = get_num_cores();

    // Use all cores if no count specified.
    if (num_workers == 0) {
        num_workers = num_cores;
    }

    // Initialize misc members.
    state.num_workers = num_workers;
    state.num_critical_workers = 1;
    state.mode = PCP_FIXED;
    state.fixed_num_critical_workers = 1;
    state.is_recruiting = false;
    state.num_recruits = 0;
    state.num_arrived_recruits = 0;
    state.num_task_types = 0;
    state.num_adaption_events = 0;

    // Initialize state.graph.
    graph_init();

    // Initialize state.queue.
    queue_init();

    // Initialize state.lock.
    pthread_mutex_init(&state.lock, NULL);

    // Initialize barriers.
    state.barrier = spin_barrier_create(state.num_workers);
    state.critical_barrier = spin_barrier_create(state.num_critical_workers);

    // Initialize state.workers.
    state.workers = malloc(sizeof(struct worker) * num_workers);
    for (int i = 0; i < num_workers; ++i) {
        state.workers[i].rank = i;
        state.workers[i].is_idle = true;
        mqueue_init(i);
        if (i != 0) {
            pthread_create(&state.workers[i].thread, NULL, worker_thread, &state.workers[i]);
        }
    }

    // Set thread affinity.
    bind_thread_to_core(0);

    printf("[INFO] Started\n");
}


void pcp_set_mode(int mode)
{
    // Update mode. 
    state.mode = mode;
    switch (mode) {
    case PCP_REGULAR:
    {
        state.fixed_num_critical_workers = 0;
        printf("[INFO] Using the regular mode\n");
    }
    break;
    
    case PCP_FIXED:
    {
        state.fixed_num_critical_workers = 1;
        printf("[INFO] Using the fixed mode\n");
    }
    break;
    
    case PCP_ADAPTIVE:
    {
        state.num_critical_workers = 1;
        printf("[INFO] Using the adaptive mode\n");
    }
    break;

    case PCP_PRESCRIBED:
    {
        printf("[INFO] Using the prescribed mode\n");
    }
    break;
    }
}


void pcp_set_num_critical_workers(int num_critical_workers)
{
    // Update thread count.
    state.fixed_num_critical_workers = num_critical_workers;
    printf("[INFO] Reserving %d core(s) in the fixed mode\n", num_critical_workers);
}


void pcp_stop(void)
{
    // Shrink the critical worker set size to 1 (i.e., only master).
    if (state.num_critical_workers > 1) {
        master_shrink(1);
    }

    // Send MSG_TERMINATE to all workers.
    master_terminate();

    // Clean up state.workers.
    for (int i = 1; i < state.num_workers; ++i) {
        pthread_join(state.workers[i].thread, NULL);
        mqueue_destroy(i);
    }
    free(state.workers);

    // Clean up state.graph.
    graph_destroy();

    // Clean up state.queue.
    queue_destroy();

    // Clean up state.lock.
    pthread_mutex_destroy(&state.lock);

    // Clean up barriers.
    spin_barrier_destroy(state.barrier);
    spin_barrier_destroy(state.critical_barrier);

    // Finalize task types.
    for (int i = 0; i < state.num_task_types; ++i) {
        struct pcp_task_type *type = state.task_types + i;
        if (type->parallel_finalize) {
            type->parallel_finalize();
        }
    }

    // Finalize thread affinity.
    thread_affinity_finalize();

    printf("[INFO] Stopped\n");
}


pcp_task_type_handle_t pcp_register_task_type(struct pcp_task_type *type)
{
    // Select position for the new type.
    int pos = state.num_task_types;

    // Bump the type counter.
    state.num_task_types += 1;

    // Store a copy of the type. 
    state.task_types[pos] = *type;

    // Call the reconfigure callback.
    if (type->parallel_reconfigure) {
        type->parallel_reconfigure(state.num_critical_workers);
    }

    // Return the task type handle.
    return (pcp_task_type_handle_t) pos;
}


void pcp_begin_graph(void)
{
    // Reset the graph.
    graph_clear();
}


void pcp_end_graph(void)
{
    // Compute task priorities.
    graph_compute_task_priorities();

    // Identify critical path.
    graph_identify_critical_path();
}


pcp_task_handle_t pcp_insert_task(pcp_task_type_handle_t type, void *arg)
{
    // Select location for the new task.
    int pos = state.graph.num_tasks;

    // Bump the task counter.
    state.graph.num_tasks += 1;

    // Enough capacity to hold the task?
    if (state.graph.num_tasks > state.graph.tasks_capacity) {
        // Double the capacity.
        state.graph.tasks_capacity *= 2;
        state.graph.tasks = (struct task**) realloc(state.graph.tasks, sizeof(struct task*) * state.graph.tasks_capacity);
    }

    // Create and store the task.
    struct task *task = task_create(type, arg);
    task->handle = (pcp_task_handle_t) pos;
    state.graph.tasks[pos] = task;

    // Return new task handle. 
    return (pcp_task_handle_t) pos;
}


void pcp_insert_dependence(pcp_task_handle_t source, pcp_task_handle_t sink)
{
    // Quick return if possible.
    if (source == PCP_TASK_HANDLE_NULL || sink == PCP_TASK_HANDLE_NULL) {
        return;
    }

    // Translate handles to tasks.
    struct task
        *from = handle2task(source),
        *to   = handle2task(sink);

    // Add outgoing edge from source.
    task_add_outgoing_edge(from, to);

    // Add incoming edge to sink.
    task_add_incoming_edge(to, from);
}


void pcp_execute_graph(void)
{
    // Warm up all cores.
    master_warmup();

    // Reset worker statistics.
    for (int i = 0; i < state.num_workers; ++i) {
        state.workers[i].time_critical_worker = 0;
        state.workers[i].time_critical_tasks = 0;
        state.workers[i].cost_critical_tasks = 0;
        state.workers[i].cost_noncritical_tasks = 0;
    }

    // Reset misc state variables.
    state.num_adaption_events = 0;

    // Adjust the number of critical workers if mode is PCP_FIXED.
    if (state.mode == PCP_FIXED) {
        if (state.fixed_num_critical_workers > state.num_critical_workers) {
            // Recruit more critical workers.
            if (state.fixed_num_critical_workers > state.num_critical_workers) {
                spin_barrier_t *next_barrier;
                next_barrier = master_grow_start(state.fixed_num_critical_workers);
                master_grow_finish(next_barrier);
            }
        } else if (state.fixed_num_critical_workers < state.num_critical_workers) {
            // Release some critical workers.
            master_shrink(state.fixed_num_critical_workers);
        }
    }

    // Identify ready tasks.
    graph_identify_ready_tasks();

    // Acquire the lock.
    lock();

    // Send ready tasks to idle workers.
    for (int rank = state.num_critical_workers; rank < state.num_workers && state.queue.num_tasks > 0; ++rank) {
        if (state.workers[rank].is_idle) {
            struct task *ready = queue_pop();
            mqueue_send_execute(rank, ready);
        }
    }

    // Release the lock.
    unlock();

    // Execute the task graph.
    master_execute_graph();
}


void pcp_view_statistics(struct pcp_statistics *stats)
{
    // execution_time
    stats->execution_time = state.execution_time;

    // busy_time
    stats->busy_time = 0.0;
    struct pcp_trace trace;
    pcp_view_trace(&trace);
    for (int i = 0; i < trace.num_events; ++i) {
        struct pcp_event *ev = &trace.events[i];
        stats->busy_time += (ev->end - ev->begin) * (double)(ev->num_cores);
    }

    // cost
    stats->cost = state.num_workers * stats->execution_time;

    // critical_worker_cost
    stats->critical_worker_cost = 0;
    for (int i = 0; i < state.num_workers; ++i) {
        stats->critical_worker_cost += state.workers[i].time_critical_worker;
    }

    // noncritical_worker_cost
    stats->noncritical_worker_cost = stats->cost - stats->critical_worker_cost;

    // critical_path_length
    stats->critical_path_length = state.workers[0].time_critical_tasks;

    // longest_path_length
    stats->longest_path_length = graph_identify_longest_path_aposteriori();

    // critical_task_cost
    stats->critical_task_cost = state.workers[0].cost_critical_tasks;

    // noncritical_task_cost
    stats->noncritical_task_cost = 0;
    for (int i = 0; i < state.num_workers; ++i) {
        stats->noncritical_task_cost += state.workers[i].cost_noncritical_tasks;
    }
}


void pcp_view_statistics_stdout(void)
{
    struct pcp_statistics stats;
    pcp_view_statistics(&stats);
    printf("\n");
    printf("STATISTICS\n");
    printf("==================================================\n");
    printf("Execution time        = %.6lf\n", stats.execution_time);
    printf("Resource utilization  = %.6lf\n", stats.busy_time / (stats.execution_time * state.num_workers));
    printf("Critical path length  = %.6lf (%5.1lf%%)\n", stats.critical_path_length, stats.critical_path_length / stats.execution_time * 100);
    printf("Longest path length   = %.6lf (%5.1lf%%)\n", stats.longest_path_length, stats.longest_path_length / stats.execution_time * 100);
    printf("Cost                  = %.6lf\n", stats.cost);
    printf("..reserved set        = %.6lf (%5.1lf%%)\n", stats.critical_worker_cost, stats.critical_worker_cost / stats.cost * 100);
    printf("....busy              = %.6lf (%5.1lf%%)\n", stats.critical_task_cost, stats.critical_task_cost / stats.critical_worker_cost * 100);
    printf("..other               = %.6lf (%5.1lf%%)\n", stats.noncritical_worker_cost, stats.noncritical_worker_cost / stats.cost * 100);
    printf("....busy              = %.6lf (%5.1lf%%)\n", stats.noncritical_task_cost, stats.noncritical_task_cost / stats.noncritical_worker_cost * 100);
    printf("Task cost             = %.6lf (%5.1lf%%)\n", stats.noncritical_task_cost + stats.critical_task_cost, (stats.noncritical_task_cost + stats.critical_task_cost) / stats.cost * 100);
    printf("Task cost per core    = %.6lf (%5.1lf%%)\n", (stats.noncritical_task_cost + stats.critical_task_cost) / state.num_workers, (stats.noncritical_task_cost + stats.critical_task_cost) / state.num_workers / stats.execution_time * 100);
    printf("==================================================\n");
}


void pcp_view_trace(struct pcp_trace *trace)
{
    // Find time base.
    double base = state.graph.tasks[0]->time_start;
    for (int i = 1; i < state.graph.num_tasks; ++i) {
        double tm = state.graph.tasks[i]->time_start;
        if (tm < base) {
            base = tm;
        }
    }
    
    // Assemble a trace.
    trace->num_events = state.graph.num_tasks;
    trace->events = (struct pcp_event*) malloc(sizeof(struct pcp_event) * trace->num_events);
    for (int i = 0; i < state.graph.num_tasks; ++i) {
        struct task *task = state.graph.tasks[i];
        struct pcp_event *ev = &trace->events[i];
        ev->task_type = task->type;
        ev->task = task->handle;
        ev->first_core = task->first_core;
        ev->num_cores = task->num_cores;
        ev->begin = task->time_start - base;
        ev->end = task->time_end - base;
        ev->is_critical = task->is_critical;
    }
    trace->num_adaption_events = state.num_adaption_events;
    trace->adaption_events = (struct pcp_adaption_event*) malloc(sizeof(struct pcp_adaption_event) * trace->num_adaption_events);
    for (int i = 0; i < trace->num_adaption_events; ++i) {
        trace->adaption_events[i] = state.adaption_events[i];
        trace->adaption_events[i].time -= base;
        if (trace->adaption_events[i].time < 0.0) {
            // Fix inaccurate timings.
            trace->adaption_events[i].time = 0.0;
        }
    }
}


void pcp_view_trace_tikz(void)
{
    struct pcp_trace trace;
    pcp_view_trace(&trace);

    double length = 0;
    double width = 100;
    char color[64];

    // Find length.
    for (int i = 0; i < trace.num_events; ++i) {
        if (trace.events[i].end > length) {
            length = trace.events[i].end;
        }
    }

    // Compute horizontal scale factor.
    // scale * length = width => scale = width / length
    float scale = (float) (width / length);

    // Open file.
    FILE *fp = fopen("trace.tex", "w");
    if (fp == NULL) {
        free(trace.events);
        free(trace.adaption_events);
        return;
    }

    // Print header.
    fprintf(fp, "\\documentclass{standalone}\n");
    fprintf(fp, "\\usepackage{tikz}\n");
    fprintf(fp, "\\usetikzlibrary{snakes}\n");
    fprintf(fp, "\\begin{document}\n");
    fprintf(fp, "\\begin{tikzpicture}[y=-1cm]\n");

    // Outline critical workers.
    {
        // \draw (0,0) -- (0,1) [-- (x,y1) -- (x,y2)] -- (e,y) -- (e,0) -- cycle;
        int count = 1;
        fprintf(fp, "\\draw [dashed, fill=green!50] (0,0) -- (0,1)");
        for (int i = 0; i < trace.num_adaption_events; ++i) {
            double when = scale * trace.adaption_events[i].time;
            int new_count = trace.adaption_events[i].count;
            fprintf(fp, " -- (%.2lf,%d) -- (%.2lf,%d)", when, count, when, new_count);
            count = new_count;
        }
        fprintf(fp, " -- (%.2lf,%d) -- (%.2lf,0) -- cycle;\n", width, count, width);
    }

    // Outline non-critical workers.
    {
        // \draw (0,p) -- (0,1) -- [-- (x,y1) -- (x,y2)] -- (e,y) -- (e,p) -- cycle;
        int count = 1;
        fprintf(fp, "\\draw [dashed, fill=blue!50] (0,%d) -- (0,1)", state.num_workers);
        for (int i = 0; i < trace.num_adaption_events; ++i) {
            double when = scale * trace.adaption_events[i].time;
            int new_count = trace.adaption_events[i].count;
            fprintf(fp, " -- (%.2lf,%d) -- (%.2lf,%d)", when, count, when, new_count);
            count = new_count;
        }
        fprintf(fp, " -- (%.2lf,%d) -- (%.2lf,%d) -- cycle;\n", width, count, width, state.num_workers);
    }
    
    // Print task events.
    for (int i = 0; i < trace.num_events; ++i) {
        struct pcp_event *ev = &trace.events[i];
        task_type_2_color(ev->task_type, color);
        float y0 = ev->first_core;
        float y1 = ev->first_core + ev->num_cores;
        y0 += 0.2;
        y1 -= 0.2;
        float x0 = scale * ev->begin;
        float x1 = scale * ev->end;
        fprintf(fp, "\\draw [%s, fill=%s] (%.2f,%.2f) rectangle (%.2f,%.2f);\n", color, ev->is_critical ? "black!50" : "black!25", x0, y0, x1, y1);
    }

    // Print axis.
    fprintf(fp, "\\draw (0,-0.5) -- (100,-0.5);\n");
    fprintf(fp, "\\draw[snake=ticks, segment length=10cm] (0,-0.5) -- (100.1,-0.5);\n");

    // Print time as axis labels at 0%, 50% and 100%.
    fprintf(fp, "\\node () at (0,-1){0};\n");
    fprintf(fp, "\\node () at (50,-1){%.6lf};\n", length/2.0);
    fprintf(fp, "\\node () at (100,-1){%.6lf};\n", length);

    // Print footer.
    fprintf(fp, "\\end{tikzpicture}\n");
    fprintf(fp, "\\end{document}\n");

    free(trace.events);
    free(trace.adaption_events);
    fclose(fp);
}
