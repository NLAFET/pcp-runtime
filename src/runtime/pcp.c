#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/time.h>
#include <hwloc.h>

#include "pcp.h"








////////////////////////////////////////////////////////////////////////////////
// INTERNAL CONSTANTS
//


#define MAX_NUM_TASK_TYPES 64
#define MAX_NUM_MESSAGES 4
#define MAX_NUM_ADAPTIONS 1024










////////////////////////////////////////////////////////////////////////////////
// INTERNAL DATA STRUCTURES
//


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

    /** The number of prescribed cores (if the mode is PCP_PRESCRIBED). */
    int prescribed_num_cores;

    /** Timestamp for the start of task execution. */
    double time_start;

    /** Timestamp for the end of task execution. */
    double time_end;

    /** The rank of the first core assigned to the task. */
    int first_core;

    /** The number of (consecutively ranked) cores assigned to the task. */
    int num_cores;

    /** The execution time (time_end - time_start). */
    double time;

    /** The longest path starting at this task. */
    double longest_path;

    /** The second longest path starting at this task. */
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


/**
 * Commands sent from master to all slaves in the reserved set.
 */
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
            pcp_barrier_t *barrier;
        } shrink;

        /** The body of a command of type CMD_GROW. */
        struct
        {
            /** The new larger barrier. */
            pcp_barrier_t *barrier;
        } grow;
    } body;
};


enum message_type { MSG_EXECUTE, MSG_RECRUIT, MSG_TERMINATE, MSG_WARMUP };


/**
 * Messages sent between threads.
 */
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
            pcp_barrier_t *barrier;
        } recruit;
    } body;
};


/**
 * Message queue for one thread.
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
    double time_critical;

    /** Time spent in critical tasks. */
    double cost_critical;

    /** Time spent in non-critical tasks. */
    double cost_noncritical;

    /** Time spent in the reserved set. */
    double time_reserved;
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
    pcp_barrier_t *barrier_global;

    /** Barrier for synchronization within the reserved set. */
    pcp_barrier_t *barrier_reserved;

    /** Command structure for master -> slave communiction within the reserved set. */
    struct command command;

    /** The number of workers (including the master). */
    int num_workers;

    /** The current size of the reserved set. */
    int reserved_set_size;

    /** Mode of execution. */
    int mode;

    /** If mode == PCP_FIXED, the specified size of the reserved set. */
    int fixed_reserved_set_size;

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









////////////////////////////////////////////////////////////////////////////////
// INTERNAL FILE-SCOPE VARIABLES
// 



/** The runtime system state. */
static struct state state;


/** Hardware topology for thread affinity. */
static hwloc_topology_t topology;


/** The nominal clock frequency of a core. */
static int nominal_clock_frequency = 0;












////////////////////////////////////////////////////////////////////////////////
// UTILITY FUNCTION DEFINITIONS
//


static double gettime(void);
static void lock(void);
static void unlock(void);
static void task_type_2_color(pcp_task_type_handle_t task_type, char *color);


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
// FREQUENCY FUNCTION DEFINITIONS
//


static void freq_estimate_nominal(void);
static int freq_get(int core);
static void freq_warmup(int core);


/**
 * Estimates the nominal clock frequency.
 *
 * Sets the nominal_clock_frequency variable.
 */
static void freq_estimate_nominal(void)
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


/**
 * Reads the current (approximate) clock frequency of a core.
 *
 * @param [in] core The core of interest.
 *
 * @return The current frequency in KHz. 
 */
static int freq_get(int core)
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


/**
 * Warmup a specific core to the nominal clock frequency.
 *
 * @param [in] core The core on which the caller is running.
 */
static void freq_warmup(int core)
{
    // Synchronize.
    pcp_barrier_wait(state.barrier_global);

    // Warm up.
    while (freq_get(core) < 0.99 * nominal_clock_frequency) {
        // empty
    }
    
    // Synchronize.
    pcp_barrier_wait(state.barrier_global);
}













////////////////////////////////////////////////////////////////////////////////
// AFFINITY FUNCTION DEFINITIONS
//


static int affinity_get_num_cores(void);
static void affinity_init(void);
static void affinity_finalize(void);
static void affinity_bind(int core);


/**
 * Returns the number of cores in the system.
 *
 * @return The total number of cores.
 */
static int affinity_get_num_cores(void)
{
    int cnt = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_CORE);
    return cnt;
}


/**
 * Initializes the thread affinity module.
 */
static void affinity_init(void)
{
    hwloc_topology_init(&topology);
    hwloc_topology_load(topology);
    printf("[INFO] Found %d cores\n", affinity_get_num_cores());
}


/**
 * Finalizes the thread affinity module.
 */
static void affinity_finalize(void)
{
    hwloc_topology_destroy(topology);
}


/**
 * Bind the caller to a specified core.
 *
 * @param [in] core The core to bind the caller to.
 */
static void affinity_bind(int core)
{
    int num_cores = affinity_get_num_cores();
    hwloc_obj_t obj;
    obj = hwloc_get_obj_by_type(topology, HWLOC_OBJ_CORE, core);
    hwloc_set_cpubind(topology, obj->cpuset, HWLOC_CPUBIND_THREAD);
}












////////////////////////////////////////////////////////////////////////////////
// ADAPTION TRACE FUNCTION DEFINITIONS
//


static void adaption_trace_add_event(int size);


/**
 * Add adaption event at this time.
 *
 * @param size The new size of the reserved set.
 */
static void adaption_trace_add_event(int size)
{
    if (state.num_adaption_events < MAX_NUM_ADAPTIONS) {
        int pos = state.num_adaption_events;
        state.num_adaption_events += 1;
        state.adaption_events[pos].time = gettime();
        state.adaption_events[pos].size = size;
    }
}










////////////////////////////////////////////////////////////////////////////////
// COMMAND FUNCTION DEFINITIONS
//


static struct command cmd_wait(pcp_barrier_t *barrier);
static void cmd_ack(pcp_barrier_t *barrier);
static void cmd_bcast_execute(pcp_barrier_t *barrier, struct task *task);
static void cmd_bcast_shrink(pcp_barrier_t *barrier, pcp_barrier_t *small_barrier);
static void cmd_bcast_grow(pcp_barrier_t *barrier, pcp_barrier_t *large_barrier);
static void cmd_bcast_wait(pcp_barrier_t *barrier);


/**
 * Waits for a command.
 *
 * @param barrier The current barrier.
 *
 * @return The received command.
 */
static struct command cmd_wait(pcp_barrier_t *barrier)
{
    // Wait for the command.
    pcp_barrier_wait(barrier);
    
    // Return the received command.
    return state.command;
}


/**
 * Acknowledges completed processing of a command.
 *
 * @param barrier The current barrier.
 */
static void cmd_ack(pcp_barrier_t *barrier)
{
    pcp_barrier_wait(barrier);
}


/**
 * Broadcasts a CMD_EXECUTE command.
 *
 * @param barrier The barrier.
 *
 * @param task The ready task to execute.
 */
static void cmd_bcast_execute(pcp_barrier_t *barrier, struct task *task)
{
    // Populate the command structure.
    state.command.type = CMD_EXECUTE;
    state.command.body.execute.task = task;
    
    // Signal the workers.
    pcp_barrier_wait(barrier);
}


/**
 * Broadcasts a CMD_SHRINK command.
 *
 * @param barrier The current barrier.
 *
 * @param small_barrier The new smaller barrier.
 */
static void cmd_bcast_shrink(pcp_barrier_t *barrier, pcp_barrier_t *small_barrier)
{
    // Populate the command structure.
    state.command.type = CMD_SHRINK;
    state.command.body.shrink.barrier = small_barrier;
    
    // Signal the workers.
    pcp_barrier_wait(barrier);
}


/**
 * Broadcasts a CMD_GROW command.
 *
 * @param barrier The current barrier.
 *
 * @param large_barrier The new larger barrier.
 */
static void cmd_bcast_grow(pcp_barrier_t *barrier, pcp_barrier_t *large_barrier)
{
    // Populate the command structure.
    state.command.type = CMD_GROW;
    state.command.body.grow.barrier = large_barrier;
    
    // Signal the workers.
    pcp_barrier_wait(barrier);
}


/**
 * Wait for the workers to acknowledge processing of command.
 *
 * @param barrier The barrier.
 */
static void cmd_bcast_wait(pcp_barrier_t *barrier)
{
    pcp_barrier_wait(barrier);
}















//////////////////////////////////////////////////////////////////////////////// 
// MESSAGE QUEUE FUNCTION DEFINITIONS
// 


static void mqueue_init(int rank);
static void mqueue_destroy(int rank);
static struct message mqueue_wait(int rank);
static struct message *mqueue_allocate(int rank);
static void mqueue_signal(int rank);
static void mqueue_send_execute(int rank, struct task *task);
static void mqueue_send_recruit(int rank, pcp_barrier_t *barrier);
static void mqueue_send_terminate(int rank);
static void mqueue_send_warmup(int rank);


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
static void mqueue_send_recruit(int rank, pcp_barrier_t *barrier)
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
// QUEUE FUNCTION DEFINITIONS
//


static void queue_init(void);
static void queue_destroy(void);
static void queue_push(struct task *task);
static struct task *queue_pop(void);


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
// TASK FUNCTION DEFINITIONS
// 


static struct task *task_from_handle(pcp_task_handle_t handle);
static struct task *task_create(pcp_task_type_handle_t type, void *arg);
static void task_destroy(struct task *task);
static void task_add_outgoing_edge(struct task *task, struct task *dest);
static void task_add_incoming_edge(struct task *task, struct task *src);
static void task_distribute_ready(void);
static void task_post_process(struct task *task);
static void task_execute_seq(struct task *task, int rank);
static void task_execute_par(struct task *task, int rank);


/**
 * Translate task handle to task pointer.
 *
 * @param [in] handle The task handle.
 *
 * @return The task pointer.
 */
static struct task *task_from_handle(pcp_task_handle_t handle)
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
    task->arg                  = arg;
    task->type                 = type;
    task->handle               = PCP_TASK_HANDLE_NULL;
    task->priority             = 0;     
    task->seqnr                = state.graph.num_tasks;
    task->num_in               = 0;
    task->num_in_ready         = 0;
    task->num_out              = 0;      
    task->out_capacity         = 16;
    task->critical_edge        = -1;
    task->is_critical          = false;
    task->is_executed          = false;
    task->prescribed_num_cores = -1; 
    task->out                  = (struct task**) malloc(sizeof(struct task*) * task->out_capacity);
    task->time_start           = 0;
    task->time_end             = 0;
    task->first_core           = -1;
    task->num_cores            = 0;
    task->time                 = 0;
    task->longest_path         = 0;
    task->second_longest_path  = 0;
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


/**
 * Distribute ready tasks to idle workers.
 */
static void task_distribute_ready(void)
{
    // Send tasks to all idle workers.
    for (int rank = state.reserved_set_size;
         rank < state.num_workers && state.queue.num_tasks > 0;
         ++rank) {
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
        if (state.reserved_set_size == 0) {
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
            if (succ->is_critical && state.reserved_set_size > 0) {
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


/**
 * Execute a task sequentially.
 *
 * @param [inout] task The task.
 *
 * @param [in] rank The rank of the caller
 */
static void task_execute_seq(struct task *task, int rank)
{
    // Record start time.
    task->time_start = gettime();
    task->first_core = rank;
    task->num_cores = 1;

    // Execute sequentially.
    struct pcp_task_type *type = state.task_types + task->type;
    type->sequential_impl(task->arg);
    
    // Record end time.
    task->time_end = gettime();
    task->time = task->time_end - task->time_start;
}


/**
 * Execute a task in parallel.
 *
 * @param [inout] task The task.
 *
 * @param [in] rank The rank of the caller.
 */
static void task_execute_par(struct task *task, int rank)
{
    if (rank == 0) {
        // Record start time.
        task->time_start = gettime();
        task->first_core = rank;
        task->num_cores = state.reserved_set_size;

        // Broadcast an CMD_EXECUTE command.
        cmd_bcast_execute(state.barrier_reserved, task);
    }

    // Execute in parallel.
    struct pcp_task_type *type = state.task_types + task->type;
    type->parallel_impl(task->arg, state.reserved_set_size, rank);

    if (rank == 0) {
        // Wait for the task to complete.
        cmd_bcast_wait(state.barrier_reserved);

        // Record end time.
        task->time_end = gettime();
        task->time = task->time_end - task->time_start;
    }
}














////////////////////////////////////////////////////////////////////////////////
// GRAPH FUNCTION DEFINITIONS
//


static void graph_init(void);
static void graph_clear(void);
static void graph_destroy(void);
static void graph_identify_ready_tasks(bool include_critical);
static double graph_identify_longest_path_aposteriori(void);
static void graph_identify_critical_path(void);
static float graph_compute_task_priorities_recursively(struct task *root);
static void graph_compute_task_priorities(void);


/**
 * Allocates memory for the graph.
 */
static void graph_init(void)
{
    state.graph.tasks_capacity     = 1024;
    state.graph.tasks              = (struct task**) malloc(sizeof(struct task*) * state.graph.tasks_capacity);
    state.graph.num_tasks          = 0;
    state.graph.num_executed_tasks = 0;
}


/**
 * Resets/empties the graph.
 */
static void graph_clear(void)
{
    for (int i = 0; i < state.graph.num_tasks; ++i) {
        task_destroy(state.graph.tasks[i]);
    }
    state.graph.num_tasks = 0;
    state.graph.num_executed_tasks = 0;
}


/**
 * Frees resources allocated to the graph.
 */
static void graph_destroy(void)
{
    for (int i = 0; i < state.graph.num_tasks; ++i) {
        task_destroy(state.graph.tasks[i]);
    }
    free(state.graph.tasks);
    state.graph.tasks = NULL;
}


/**
 * Identify the initially ready tasks in the graph
 *
 * Ready tasks are pushed to the shared queue.
 *
 * @param [in] include_critical If set, then also include ready critical tasks.
 */
static void graph_identify_ready_tasks(bool include_critical)
{
    for (int i = 0; i < state.graph.num_tasks; ++i) {
        struct task *task = state.graph.tasks[i];

        // Skip critical tasks.
        if (!include_critical && task->is_critical) {
            continue;
        }

        // Ready?
        if (task->num_in == 0) {
            queue_push(task);
        }
    }
}


/**
 * Find the longest path.
 *
 * @return The length of the longest path.
 */
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


/**
 * Identify the critical path based on the number of tasks in the path.
 *
 * The found critical path is encoded in the task structures.
 */
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


/**
 * Recursively compute task priorities for the subgraph rooted at a given task.
 *
 * @param [inout] root The task at the root of the subgraph.
 *
 * @return The priority assigned to root.
 */
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


/**
 * Compute priorities of all tasks.
 */
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
// MASTER FUNCTION DEFINITIONS
//


static void master_warmup(void);
static void master_reconfigure(void);
static pcp_barrier_t *master_grow_start(int size);
static void master_grow_finish(pcp_barrier_t *next_barrier);
static void master_try_grow_finish(pcp_barrier_t *next_barrier);
static void master_shrink(int size);
static void master_terminate(void);
static void master_execute_graph_regular(void);
static void master_execute_graph(void);


/**
 * Warm up all threads.
 */
static void master_warmup(void)
{
    lock();
    for (int i = 1; i < state.num_workers; ++i) {
        mqueue_send_warmup(i);
    }
    unlock();
    freq_warmup(0);
}


/**
 * Reconfigures all the task types (where applicable).
 *
 * Call this function after changing state.reserved_set_size.
 */
static void master_reconfigure(void)
{
    for (int i = 0; i < state.num_task_types; ++i) {
        struct pcp_task_type *type = state.task_types + i;
        if (type->parallel_reconfigure) {
            type->parallel_reconfigure(state.reserved_set_size);
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
static pcp_barrier_t *master_grow_start(int size)
{
    pcp_barrier_t *next_barrier;
    
    // Add to trace.
    adaption_trace_add_event(size);

    // Store information about recruitment process.
    state.is_recruiting = true;
    state.num_recruits = size - state.reserved_set_size;
    state.num_arrived_recruits = 0;
            
    // Create new larger barrier.
    next_barrier = pcp_barrier_create(state.reserved_set_size + state.num_recruits);

    // Send MSG_RECRUIT message to all recruits.
    for (int i = state.reserved_set_size; i < state.reserved_set_size + state.num_recruits; ++i) {
        mqueue_send_recruit(i, next_barrier);
    }

    return next_barrier;
}


/**
 * Finish recruitment process.
 *
 * @param next_barrier The new larger barrier.
 */
static void master_grow_finish(pcp_barrier_t *next_barrier)
{
    // Broadcast CMD_GROW command.
    cmd_bcast_grow(state.barrier_reserved, next_barrier);

    // Wait for acknowledgment.
    cmd_bcast_wait(state.barrier_reserved);

    // Safely destroy the old barrier.
    pcp_barrier_wait_and_destroy(state.barrier_reserved);

    // Replace the barrier.
    state.barrier_reserved = next_barrier;

    // Finish the recruitment process.
    state.is_recruiting = false;
    state.reserved_set_size += state.num_recruits;
    state.num_recruits = 0;
    state.num_arrived_recruits = 0;

    // Reconfigure the task types.
    master_reconfigure();
}


/**
 * Finish recruitment process if possible.
 *
 * @param next_barrier The new larger barrier.
 */
static void master_try_grow_finish(pcp_barrier_t *next_barrier)
{
    if (state.is_recruiting && state.num_arrived_recruits == state.num_recruits) {
        master_grow_finish(next_barrier);
    }
}


/**
 * Shrink the size of the reserved set.
 *
 * @param size The new size.
 */
static void master_shrink(int size)
{
    // Quick return if possible.
    if (state.reserved_set_size == size) {
        return;
    }

    // Add adaption event.
    adaption_trace_add_event(size);

    // Update the reserved set size.
    state.reserved_set_size = size;

    // Create a smaller barrier.
    pcp_barrier_t *small_barrier = pcp_barrier_create(size);

    // Broadcast the CMD_SHRINK command.
    cmd_bcast_shrink(state.barrier_reserved, small_barrier);

    // Wait for workers to process command.
    cmd_bcast_wait(state.barrier_reserved);

    // Safely destroy the old barrier.
    pcp_barrier_wait_and_destroy(state.barrier_reserved);

    // Replace the barrier with the smaller one.
    state.barrier_reserved = small_barrier;

    // Reconfigure the task types.
    master_reconfigure();
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


/**
 * Master's algorithm to execute the graph in the PCP_REGULAR mode.
 */
static void master_execute_graph_regular(void)
{
    struct worker *me = &state.workers[0];
    bool terminated = false;

    double execution_time = gettime();

    // Mark self as idle.
    me->is_idle = true;
    
    // Identify ready tasks.
    graph_identify_ready_tasks(true);

    // Distribute ready tasks.
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
            //
            // Execute the task.
            //

            // Get the task.
            struct task *task = msg.body.execute.task;
            
            // Sequentially execute msg->body.execute.task.
            task_execute_seq(task, me->rank);

            // Accumulate coset of exeucting tasks.
            me->cost_noncritical += task->time_end - task->time_start;
            
            //
            // Post-process the task.
            //

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
    if (state.reserved_set_size == 0) {
        master_execute_graph_regular();
        return;
    }
    
    pcp_barrier_t *next_barrier = NULL;
    
    // Identify ready tasks.
    graph_identify_ready_tasks(false);

    // Distribute ready tasks.
    task_distribute_ready();

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

        // Run control logic for PCP_PRESCRIBED mode.
        if (state.mode == PCP_PRESCRIBED && state.is_recruiting == false) {
            if (task->prescribed_num_cores > state.reserved_set_size) {
                next_barrier = master_grow_start(task->prescribed_num_cores);
            } else if (task->prescribed_num_cores < state.reserved_set_size) {
                master_shrink(task->prescribed_num_cores);
            }
        }

        //
        // Wait for the task to become ready.
        //

        // Loop until ready.
        if (task->num_in_ready != task->num_in) {
            while (task->num_in_ready != task->num_in) {
                pthread_cond_wait(&state.cv_master, &state.lock);
            }
        }

        // Run control logic for PCP_PRESCRIBED mode.
        if (state.mode == PCP_PRESCRIBED) {
            master_try_grow_finish(next_barrier);
        }
            
        // Release the lock.
        unlock();
        
        //
        // Execute the task.
        //

        // Get the task implementations.
        struct pcp_task_type *task_type = state.task_types + (int) task->type;

        // Determine how to execute the task (sequentially or in parallel).
        bool sequentially = true;
        if (task_type->parallel_impl == NULL || state.reserved_set_size == 1) {
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
        state.workers[0].cost_critical += task->time * (sequentially ? 1 : state.reserved_set_size);
        state.workers[0].time_critical += task->time;

        //
        // Post-process the task.
        //

        // Acquire the lock.
        lock();
        
        // Post-process the task.
        task_post_process(task);

        // Release the lock.
        unlock();

        //
        // Advance to the next critical task.
        //
        
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

    // Get rid of all slaves from the reserved set.
    if (state.reserved_set_size > 1) { 
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
    state.workers[0].time_reserved += execution_time;
    
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








////////////////////////////////////////////////////////////////////////////////
// WORKER FUNCTION DEFINITIONS
// 


static void worker_reserved(struct worker *me, pcp_barrier_t *barrier);
static void *worker_other(void *ptr);


/**
 * Worker's algorithm when in the reserved set.
 *
 * @param me The worker state.
 *
 * @param barrier The barrier for synchronization.
 */
static void worker_reserved(struct worker *me, pcp_barrier_t *barrier)
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
            pcp_barrier_wait_and_destroy(barrier);

            // Am I one of the critical workers?
            if (me->rank < state.reserved_set_size) {
                // Yes.

                // Use the new smaller barrier.
                barrier = cmd.body.shrink.barrier;
            } else {
                // No.

                // Account for time as critical worker.
                tm = gettime() - tm;
                me->time_reserved += tm;

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
            pcp_barrier_wait_and_destroy(barrier);
            
            // Use the new larger barrier.
            barrier = cmd.body.grow.barrier;
        }
        break;
        }
    }
}


/**
 * Worker's algorithm when not in the reserved set.
 *
 * @param ptr The worker state of type struct worker. 
 */
static void *worker_other(void *ptr)
{
    struct worker *me = (struct worker*) ptr;

    // Set thread affinity.
    affinity_bind(me->rank);

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
            //
            // Execute the task.
            //

            // Get the task.
            struct task *task = msg.body.execute.task;

            // Sequentially execute msg->body.execute.task.
            task_execute_seq(task, me->rank);

            // Accumulate coset of exeucting tasks.
            me->cost_noncritical += task->time_end - task->time_start;
            
            //
            // Post-process the task.
            //

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

            // Run the algorithm for the reserved set and then continue.
            worker_reserved(me, msg.body.recruit.barrier);

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
            freq_warmup(me->rank);
            break;
        }
    }

    return NULL;
}













////////////////////////////////////////////////////////////////////////////////
// API FUNCTION DEFINITIONS
//


double pcp_get_time(void)
{
    return gettime();
}


void pcp_start(int num_workers)
{
    printf("[INFO] Starting\n");
    
    // Estimate nominal core frequency.
    freq_estimate_nominal();

    // Initialize thread affinity.
    affinity_init();

    // Get the number of cores.
    int num_cores = affinity_get_num_cores();

    // Use all cores if no count specified.
    if (num_workers == 0) {
        num_workers = num_cores;
    }

    // Initialize misc members.
    state.num_workers             = num_workers;
    state.reserved_set_size       = 1;
    state.mode                    = PCP_FIXED;
    state.fixed_reserved_set_size = 1;
    state.is_recruiting           = false;
    state.num_recruits            = 0;
    state.num_arrived_recruits    = 0;
    state.num_task_types          = 0;
    state.num_adaption_events     = 0;

    // Initialize state.graph.
    graph_init();

    // Initialize state.queue.
    queue_init();

    // Initialize state.lock.
    pthread_mutex_init(&state.lock, NULL);

    // Initialize barriers.
    state.barrier_global   = pcp_barrier_create(state.num_workers);
    state.barrier_reserved = pcp_barrier_create(state.reserved_set_size);

    // Initialize state.workers.
    state.workers = malloc(sizeof(struct worker) * num_workers);
    for (int i = 0; i < num_workers; ++i) {
        state.workers[i].rank = i;
        state.workers[i].is_idle = true;
        mqueue_init(i);
        if (i != 0) {
            pthread_create(&state.workers[i].thread, NULL, worker_other, &state.workers[i]);
        }
    }

    // Set thread affinity.
    affinity_bind(0);

    printf("[INFO] Started\n");
}


void pcp_set_mode(int mode)
{
    // Update mode. 
    state.mode = mode;
    switch (mode) {
    case PCP_REGULAR:
    {
        state.fixed_reserved_set_size = 0;
        printf("[INFO] Using the regular mode\n");
    }
    break;
    
    case PCP_FIXED:
    {
        state.fixed_reserved_set_size = 1;
        printf("[INFO] Using the fixed mode\n");
    }
    break;
    
    case PCP_ADAPTIVE:
    {
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


void pcp_set_reserved_set_size(int size)
{
    // Update thread count.
    state.fixed_reserved_set_size = size;
    printf("[INFO] Reserving %d core(s) in the fixed mode\n", size);
}


void pcp_stop(void)
{
    // Shrink the reserved set to 1 (i.e., only master).
    if (state.reserved_set_size > 1) {
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
    pcp_barrier_destroy(state.barrier_global);
    pcp_barrier_destroy(state.barrier_reserved);

    // Finalize task types.
    for (int i = 0; i < state.num_task_types; ++i) {
        struct pcp_task_type *type = state.task_types + i;
        if (type->parallel_finalize) {
            type->parallel_finalize();
        }
    }

    // Finalize thread affinity.
    affinity_finalize();

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
        type->parallel_reconfigure(state.reserved_set_size);
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
        *from = task_from_handle(source),
        *to   = task_from_handle(sink);

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
        state.workers[i].time_reserved    = 0;
        state.workers[i].time_critical    = 0;
        state.workers[i].cost_critical    = 0;
        state.workers[i].cost_noncritical = 0;
    }

    // Reset misc state variables.
    state.num_adaption_events = 0;

    // Adjust the size of the reserved set if mode is PCP_FIXED or PCP_REGULAR.
    if (state.mode == PCP_FIXED || state.mode == PCP_REGULAR) {
        if (state.fixed_reserved_set_size > state.reserved_set_size) {
            // Recruit more workers into the reserved set.
            if (state.fixed_reserved_set_size > state.reserved_set_size) {
                pcp_barrier_t *next_barrier;
                next_barrier = master_grow_start(state.fixed_reserved_set_size);
                master_grow_finish(next_barrier);
            }
        } else if (state.fixed_reserved_set_size < state.reserved_set_size) {
            // Release some workers from the reserved set.
            master_shrink(state.fixed_reserved_set_size);
        }
    }

    // Execute the task graph.
    master_execute_graph();
}


void pcp_view_statistics(struct pcp_statistics *stats)
{
    // execution_time
    stats->execution_time = state.execution_time;

    // cost
    stats->cost = state.num_workers * stats->execution_time;

    // cost_reserved
    stats->cost_reserved = 0;
    for (int i = 0; i < state.num_workers; ++i) {
        stats->cost_reserved += state.workers[i].time_reserved;
    }

    // cost_other
    stats->cost_other = stats->cost - stats->cost_reserved;

    // cost_critical
    stats->cost_critical = state.workers[0].cost_critical;

    // cost_noncritical
    stats->cost_noncritical = 0;
    for (int i = 0; i < state.num_workers; ++i) {
        stats->cost_noncritical += state.workers[i].cost_noncritical;
    }

    // critical_path_length
    stats->critical_path_length = state.workers[0].time_critical;

    // longest_path_length
    stats->longest_path_length = graph_identify_longest_path_aposteriori();
}


void pcp_view_statistics_stdout(void)
{
    struct pcp_statistics stats;
    pcp_view_statistics(&stats);
    printf("\n");
    printf("STATISTICS\n");
    printf("==================================================\n");
    printf("Execution time        = %.6lf\n", stats.execution_time);
    printf("Resource utilization  = %.6lf\n", (stats.cost_critical + stats.cost_noncritical) / (stats.execution_time * state.num_workers));
    printf("Critical path length  = %.6lf (%5.1lf%%)\n", stats.critical_path_length, stats.critical_path_length / stats.execution_time * 100);
    printf("Longest path length   = %.6lf (%5.1lf%%)\n", stats.longest_path_length, stats.longest_path_length / stats.execution_time * 100);
    printf("Cost                  = %.6lf\n", stats.cost);
    printf("..reserved            = %.6lf (%5.1lf%%)\n", stats.cost_reserved, stats.cost_reserved / stats.cost * 100);
    printf("....busy              = %.6lf (%5.1lf%%)\n", stats.cost_critical, stats.cost_critical / stats.cost_reserved * 100);
    printf("..other               = %.6lf (%5.1lf%%)\n", stats.cost_other, stats.cost_other / stats.cost * 100);
    printf("....busy              = %.6lf (%5.1lf%%)\n", stats.cost_noncritical, stats.cost_noncritical / stats.cost_other * 100);
    printf("Task cost             = %.6lf (%5.1lf%%)\n", stats.cost_noncritical + stats.cost_critical, (stats.cost_noncritical + stats.cost_critical) / stats.cost * 100);
    printf("Task cost per core    = %.6lf (%5.1lf%%)\n", (stats.cost_noncritical + stats.cost_critical) / state.num_workers, (stats.cost_noncritical + stats.cost_critical) / state.num_workers / stats.execution_time * 100);
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

    // Outline the reservet set.
    {
        // \draw (0,0) -- (0,1) [-- (x,y1) -- (x,y2)] -- (e,y) -- (e,0) -- cycle;
        int size = 1;
        fprintf(fp, "\\draw [dashed, fill=green!50] (0,0) -- (0,1)");
        for (int i = 0; i < trace.num_adaption_events; ++i) {
            double when = scale * trace.adaption_events[i].time;
            int new_size = trace.adaption_events[i].size;
            fprintf(fp, " -- (%.2lf,%d) -- (%.2lf,%d)", when, size, when, new_size);
            size = new_size;
        }
        fprintf(fp, " -- (%.2lf,%d) -- (%.2lf,0) -- cycle;\n", width, size, width);
    }

    // Outline the non-reserved set.
    {
        // \draw (0,p) -- (0,1) -- [-- (x,y1) -- (x,y2)] -- (e,y) -- (e,p) -- cycle;
        int size = 1;
        fprintf(fp, "\\draw [dashed, fill=blue!50] (0,%d) -- (0,1)", state.num_workers);
        for (int i = 0; i < trace.num_adaption_events; ++i) {
            double when = scale * trace.adaption_events[i].time;
            int new_size = trace.adaption_events[i].size;
            fprintf(fp, " -- (%.2lf,%d) -- (%.2lf,%d)", when, size, when, new_size);
            size = new_size;
        }
        fprintf(fp, " -- (%.2lf,%d) -- (%.2lf,%d) -- cycle;\n", width, size, width, state.num_workers);
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
