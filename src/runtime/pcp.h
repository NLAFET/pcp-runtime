#ifndef PCP_H
#define PCP_H


#include <stdint.h>
#include <stdbool.h>

#include "spin-barrier.h"



#define PCP_REGULAR 0
#define PCP_FIXED 1
#define PCP_ADAPTIVE 2
#define PCP_PRESCRIBED 3


/**
 * Describes the implementations of a task type.
 *
 * @see pcp_register_task_type
 * @see pcp_insert_task
 */
struct pcp_task_type
{
    /**
     * The sequential implementation.
     *
     * The argument points to the user-defined task instance parameters.
     */
    void (*sequential_impl)(void*);

    /**
     * The parallel implementation.
     *
     * The first argument points to the user-defined task instance
     * parameters.  The second argument is the number of threads
     * participating the parallel execution.  The third argument is
     * the rank of the caller.
     */
    void (*parallel_impl)(void*, int, int);

    /**
     * Called when the master changes the number of threads for the
     * parallel implementation.
     */
    void (*parallel_reconfigure)(int);

    /**
     * Called when the runtime system shuts down. 
     */
    void (*parallel_finalize)(void);
};


/**
 * Refers to a registered task type.
 *
 * @see pcp_register_task_type
 * @see pcp_insert_task
 */
typedef int pcp_task_type_handle_t;


/**
 * Refers to a task in the graph.
 *
 * @see pcp_insert_task
 * @see pcp_insert_dependence
 */
typedef int pcp_task_handle_t;


static const pcp_task_handle_t PCP_TASK_HANDLE_NULL = -1;


/**
 * Represents when and where a task was executed.
 *
 * @see pcp_trace
 * @see pcp_view_trace
 */
struct pcp_event
{
    /** Handle to the task's type. */
    pcp_task_type_handle_t task_type;
    
    /** Handle to the task. */
    pcp_task_handle_t task;

    /** Set if the task is critical. */
    bool is_critical;
    
    /** The rank of the first core executing the task. */
    int first_core;
    
    /** The number of consecutively ranked cores executing the task. */
    int num_cores;
    
    /** Timestamp for the start of the execution. */
    double begin;
    
    /** Timestamp for the end of the execution. */
    double end;
};


/**
 * Represents a change in the number of critical workers.
 *
 * @see pcp_trace
 * @see pcp_view_trace
 */
struct pcp_adaption_event
{
    /** When the event occurred. */
    double time;

    /** The new critical worker count. */
    int count;
};


/**
 * Represents an execution trace.
 *
 * @see pcp_event
 * @see pcp_view_trace
 */
struct pcp_trace
{
    /** The number of task events. */
    int num_events;

    /** The task events. */
    struct pcp_event *events;

    /** The number of adaption events. */
    int num_adaption_events;

    /** The adaption events. */
    struct pcp_adaption_event *adaption_events;
};


/**
 * Represents statistics about the execution of a task graph.
 *
 * @see pcp_view_statistics
 */
struct pcp_statistics
{
    /** The execution time in seconds. */
    double execution_time;

    /** The cumulated time spent on processing tasks in seconds. */
    double busy_time;

    /** The parallel cost in CPU seconds. */
    double cost;

    /** The part of the cost that is spent by critical workers. */
    double critical_worker_cost;

    /** The part of the cost that is spent by non-critical workers. */
    double noncritical_worker_cost;

    /** The sum of the critical task execution times. */
    double critical_path_length;

    /** The execution time of the longest path. */
    double longest_path_length;

    /** The part of the cost that is attributed to critical tasks. */
    double critical_task_cost;

    /** The part of the cost that is attributed to non-critical tasks. */
    double noncritical_task_cost;
};


/**
 * Returns the current time.
 *
 * @return The current time in seconds.
 */
double pcp_get_time(void);


/**
 * Initializes the runtime system.
 *
 * This function must be called before any other function can be
 * called.
 *
 * @param [in] num_workers The desired number of workers (including
 * the master) or -1 to use one worker per core.
 * 
 * @see pcp_stop
 */
void pcp_start(int num_workers);


// TODO Add documentation.
void pcp_set_mode(int mode);


// TODO Add documentation.
// TODO Rename to pcp_set_reserved_set_size
void pcp_set_num_critical_workers(int num_critical_workers);


/**
 * Shuts down the runtime system.
 *
 * @see pcp_start
 */
void pcp_stop(void);


/**
 * Registers a new task type.
 *
 * The task type persists until the runtime system shuts down.
 *
 * @param [in] task_type The task type descriptor.
 * 
 * @return Handle to the new task type.
 * 
 * @see pcp_task_type
 */
pcp_task_type_handle_t pcp_register_task_type(struct pcp_task_type *task_type);


/**
 * Begins the construction of a new task graph.
 *
 * @see pcp_insert_task
 * @see pcp_insert_dependence
 * @see pcp_end_graph
 */
void pcp_begin_graph(void);


/**
 * Ends the construction of a task graph.
 *
 * @see pcp_execute_graph
 */
void pcp_end_graph(void);


/**
 * Inserts a new task into the task graph.
 *
 * @param [in] task_type Handle to the task's type.
 * 
 * @param [in] arg User-defined task instance parameter to be passed
 * to the task implementations.
 * 
 * @see pcp_insert_dependence
 */
pcp_task_handle_t pcp_insert_task(pcp_task_type_handle_t task_type, void *arg);


/**
 * Inserts a task dependence.
 *
 * @param [in] source Handle to the source task.
 *
 * @param [in] sink Handle to the sink task.
 */
void pcp_insert_dependence(pcp_task_handle_t source, pcp_task_handle_t sink);


/**
 * Executes the task graph. 
 */
void pcp_execute_graph(void);


/**
 * Extracts post-execution statistics.
 *
 * @param [out] stats Structure where the statistics will be written.
 */
void pcp_view_statistics(struct pcp_statistics *stats);


/**
 * Prints post-execution statistics to stdout.
 */
void pcp_view_statistics_stdout(void);


/**
 * Extracts post-execution trace.
 *
 * @param [out] trace Structure where the statistics will be written.
 *
 * @note Caller is responsible for deallocating memory.
 */
void pcp_view_trace(struct pcp_trace *trace);


/**
 * Saves trace in TikZ format to file.
 */
void pcp_view_trace_tikz(void);


#endif
