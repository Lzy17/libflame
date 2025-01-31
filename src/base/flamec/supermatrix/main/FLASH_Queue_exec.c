/*

   Copyright (C) 2014, The University of Texas at Austin

   This file is part of libflame and is available under the 3-Clause
   BSD license, which can be found in the LICENSE file at the top-level
   directory, or at http://opensource.org/licenses/BSD-3-Clause

 */

#include "FLAME.h"


#if   FLA_MULTITHREADING_MODEL == FLA_OPENMP
#ifdef FLA_ENABLE_TIDSP
#include <ti/omp/omp.h>
#else
#include <omp.h>
#endif
#elif FLA_MULTITHREADING_MODEL == FLA_PTHREADS
#include <pthread.h>
#endif

#ifdef FLA_ENABLE_WINDOWS_BUILD
#define _CRT_RAND_S
#include <stdlib.h>
#endif

//enable d2d here
#define FLA_ENABLE_D2D 1
//define function for binary encoding
#define bcode_check(var, pos) ((var) & (1<<(pos)))
#define bcode_set(var, pos) ((var) | (1<<(pos)))
#define bcode_clear(var, pos) ((var) & (~(1<<(pos))))




#ifdef FLA_ENABLE_SUPERMATRIX

#ifndef FLA_ENABLE_SCC

#define MIN_CACHE_BLOCKS  3

#ifdef FLA_ENABLE_GPU
typedef struct FLA_Obj_gpu_struct
{
	// Block stored in a GPU.
	FLA_Obj      obj;

	// Pointer to the data stored on the GPU.
	void*        buffer_gpu;

	// Whether the block is clean or dirty on the GPU.
	FLA_Bool     clean;

	// Whether the block has been requested by another GPU.
	FLA_Bool     request;

} FLA_Obj_gpu;
#endif

#ifdef FLA_ENABLE_HIP
typedef struct FLA_Obj_hip_struct
{
	// Block stored in a HIP accelerator.
	FLA_Obj      obj;

	// Pointer to the data stored on the accelerator.
	void*        buffer_hip;

	// Whether the block is clean or dirty on the accelerator.
	FLA_Bool     clean;

	// Whether the block has been requested by another accelerator.
	FLA_Bool     request;

} FLA_Obj_hip;
#endif

typedef struct FLASH_Queue_variables
{
	// A lock on the global task counter.  
	// Needed only when multithreading is enabled.
	FLA_Lock     all_lock;

	// A lock that protects the thread's waiting queue.
	// Needed only when multithreading is enabled.
	FLA_Lock*    run_lock;

	// A lock that allows threads to safely check for and place ready dependent
	// tasks on waiting queue.  Needed only when multithreading is enabled.
	FLA_Lock*    dep_lock;

	// A lock that allows threads to safely free the anti-dependency queue
	// within each block.  Needed only when multithreading is enabled.
	FLA_Lock*    war_lock;

	// A lock that allows threads to safely access the cache structures.
	// Needed only when multithreading is enabled.
	FLA_Lock*    cac_lock;

	// Number of queues.
	int          n_queues;

	// Number of caches.
	int          n_caches;

	// The number of blocks that can be stored in the cache on each thread.
	int          size;

	// LRU cache simulation of blocks.
	FLA_Obj*     cache;

	// List of blocks accessed by the first tasks.
	FLA_Obj*     prefetch;

	// The waiting queue of tasks for each thread.
	FLASH_Queue* wait_queue;

	// A global task counter that keeps track of how many tasks on the waiting
	// queue have been processed.
	int          pc;

#ifdef FLA_ENABLE_GPU
	// A lock that allows threads to safely access the cache structures.
	// Needed only when multithreading is enabled.
	FLA_Lock*    gpu_lock;

	// LRU software cache of GPU memory.
	FLA_Obj_gpu* gpu;

	// Storing the block being evicted.
	FLA_Obj_gpu* victim;

	// Temporary storage for logging blocks on GPU.
	FLA_Obj_gpu* gpu_log;

	// The size of each block to allocate on GPU.
	dim_t        block_size;      

	// The datatype of each block to allocate on GPU.
	FLA_Datatype datatype;
#endif

#ifdef FLA_ENABLE_HIP
	// A lock that allows threads to safely access the cache structures.
	// Needed only when multithreading is enabled.
	FLA_Lock*    hip_lock;

	// LRU software cache of HIP memory.
	FLA_Obj_hip* hip;

	// Storing the block being evicted.
	FLA_Obj_hip* victim;

	// Temporary storage for logging blocks on the accelerator.
	FLA_Obj_hip* hip_log;

	// The size of each block to allocate on the accelerator.
	dim_t        block_size;

	// The datatype of each block to allocate on the accelerator.
	FLA_Datatype datatype;
#endif

} FLASH_Queue_vars;


void FLASH_Queue_exec( void )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_exec

	  ----------------------------------------------------------------------------*/
{
	int          n_tasks    = FLASH_Queue_get_num_tasks();
	int          n_threads  = FLASH_Queue_get_num_threads();
	int          n_queues;
	int          n_caches;
	int          size;
	int          i;
	dim_t        block_size = FLASH_Queue_get_block_size();
	double       dtime;

	FLA_Lock*    run_lock;
	FLA_Lock*    dep_lock;
	FLA_Lock*    war_lock;
	FLA_Lock*    cac_lock;

	FLA_Obj*     cache;
	FLA_Obj*     prefetch;
	FLASH_Queue* wait_queue;

#ifdef FLA_ENABLE_GPU
#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock*    gpu_lock;
#endif
	FLA_Obj_gpu* gpu;
	FLA_Obj_gpu* victim;
	FLA_Obj_gpu* gpu_log;
	dim_t        gpu_n_blocks = FLASH_Queue_get_gpu_num_blocks();
#endif

#ifdef FLA_ENABLE_HIP
#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock*    hip_lock;
#endif
	FLA_Obj_hip* hip;
	FLA_Obj_hip* victim;
	FLA_Obj_hip* hip_log;
	dim_t        hip_n_blocks = FLASH_Queue_get_hip_num_blocks();
#endif

	// All the necessary variables for the SuperMatrix mechanism.
	FLASH_Queue_vars args;

	// If the queue is empty, return early.
	if ( n_tasks == 0 )
		return;

#ifndef FLA_ENABLE_MULTITHREADING
	// Turn off work stealing in simulation mode.
	FLASH_Queue_set_work_stealing( FALSE );
#endif

	// Query the number of user set threads per queue.
	n_queues = FLASH_Queue_get_cores_per_queue();

	// Default user setting for number of threads.
	if ( n_queues <= 0 )
	{
		// Do not use data affinity or work stealing when caching is enabled.
		if ( FLASH_Queue_get_caching() )
		{
			FLASH_Queue_set_data_affinity( FLASH_QUEUE_AFFINITY_NONE );
			FLASH_Queue_set_work_stealing( FALSE );
		}

		// Do not use work stealing when data affinity is enabled.
		if ( FLASH_Queue_get_data_affinity() != FLASH_QUEUE_AFFINITY_NONE )
		{
			FLASH_Queue_set_work_stealing( FALSE );
		}

		// Allocate different arrays if using data affinity.
		n_queues = ( FLASH_Queue_get_data_affinity() == 
				FLASH_QUEUE_AFFINITY_NONE &&
				!FLASH_Queue_get_work_stealing() ? 1 : n_threads );      
	}
	else
	{
		// Set the number of queues.
		n_queues = n_threads / n_queues;

		// Must use at least one queue.
		if ( n_queues == 0 )
			n_queues = 1;

		if ( n_queues == 1 )
		{
			// Turn off all multiple queue implementations.
			FLASH_Queue_set_data_affinity( FLASH_QUEUE_AFFINITY_NONE );
			FLASH_Queue_set_work_stealing( FALSE );
		}
		else
		{
			// Use 2D data affinity for multiple queues if nothing is set.
			if ( FLASH_Queue_get_data_affinity() == FLASH_QUEUE_AFFINITY_NONE &&
					!FLASH_Queue_get_work_stealing() )
			{
				FLASH_Queue_set_data_affinity( FLASH_QUEUE_AFFINITY_2D_BLOCK_CYCLIC );
			}
		}
	}

	// Determine the number of caches.
	n_caches = n_threads / FLASH_Queue_get_cores_per_cache();

	args.n_queues = n_queues;
	args.n_caches = n_caches;

#ifdef FLA_ENABLE_MULTITHREADING
	// Allocate memory for array of locks.
	run_lock = ( FLA_Lock* ) FLA_malloc( n_queues  * sizeof( FLA_Lock ) );
	dep_lock = ( FLA_Lock* ) FLA_malloc( n_threads * sizeof( FLA_Lock ) );
	war_lock = ( FLA_Lock* ) FLA_malloc( n_threads * sizeof( FLA_Lock ) );
	cac_lock = ( FLA_Lock* ) FLA_malloc( n_caches  * sizeof( FLA_Lock ) );

	args.run_lock = run_lock;
	args.dep_lock = dep_lock;
	args.war_lock = war_lock;
	args.cac_lock = cac_lock;

	// Initialize the all lock.
	FLA_Lock_init( &(args.all_lock) );

	// Initialize the run lock for thread i.
	for ( i = 0; i < n_queues; i++ )
	{
		FLA_Lock_init( &(args.run_lock[i]) );
	}

	// Initialize the dep and war locks for thread i.
	for ( i = 0; i < n_threads; i++ )
	{
		FLA_Lock_init( &(args.dep_lock[i]) );
		FLA_Lock_init( &(args.war_lock[i]) );
	}

	// Initialize the cac locks for each cache.
	for ( i = 0; i < n_caches; i++ )
	{
		FLA_Lock_init( &(args.cac_lock[i]) );
	}
#endif

	// The number of blocks that can fit into the cache on each thread.
	if ( block_size == 0 )
		size = MIN_CACHE_BLOCKS;
	else
		size = max( FLASH_Queue_get_cache_size() / block_size, MIN_CACHE_BLOCKS);
	args.size = size;

	// Allocate memory for cache, prefetch buffer, and waiting queue.
	cache = ( FLA_Obj* ) FLA_malloc( size * n_caches * sizeof( FLA_Obj ) );
	prefetch = ( FLA_Obj* ) FLA_malloc( size * sizeof( FLA_Obj ) );
	wait_queue = ( FLASH_Queue* ) FLA_malloc( n_queues * sizeof( FLASH_Queue ));

	args.cache = cache;
	args.prefetch = prefetch;
	args.wait_queue = wait_queue;

	// Initialize cache, prefetch buffer, and waiting queue.
	for ( i = 0; i < size * n_caches; i++ ){
		args.cache[i].base = NULL;
		//args.cache[i].thread = -1;
	}
	for ( i = 0; i < size; i++ ){
		args.prefetch[i].base = NULL;
		//args.prefetch[i].thread = -1;
	}
	for ( i = 0; i < n_queues; i++ )
	{
		args.wait_queue[i].n_tasks = 0;
		args.wait_queue[i].head = NULL;
		args.wait_queue[i].tail = NULL;
	}

	// Initialize the aggregate task counter.
	args.pc = 0;

#ifdef FLA_ENABLE_GPU
#ifdef FLA_ENABLE_MULTITHREADING
	// Allocate and initialize the gpu locks.
	gpu_lock = ( FLA_Lock* ) FLA_malloc( n_threads * sizeof( FLA_Lock ) );
	args.gpu_lock = gpu_lock;   

	for ( i = 0; i < n_threads; i++ )
		FLA_Lock_init( &(args.gpu_lock[i]) );
#endif
	// Allocate and initialize GPU software cache.
	gpu = ( FLA_Obj_gpu* ) FLA_malloc( gpu_n_blocks * n_threads * sizeof( FLA_Obj_gpu ) );
	args.gpu = gpu;

	for ( i = 0; i < gpu_n_blocks * n_threads; i++ )
	{
		args.gpu[i].obj.base   = NULL;
		args.gpu[i].buffer_gpu = NULL;
		args.gpu[i].clean      = TRUE;
		args.gpu[i].request    = FALSE;
	}

	victim = ( FLA_Obj_gpu* ) FLA_malloc( n_threads * sizeof( FLA_Obj_gpu ) );
	args.victim = victim;

	for ( i = 0; i < n_threads; i++ )
		args.victim[i].obj.base = NULL;

	gpu_log = ( FLA_Obj_gpu* ) FLA_malloc( gpu_n_blocks * n_threads * sizeof( FLA_Obj_gpu ) );
	args.gpu_log = gpu_log;
#endif

#ifdef FLA_ENABLE_HIP
#ifdef FLA_ENABLE_MULTITHREADING
	// Allocate and initialize the hip locks.
	hip_lock = ( FLA_Lock* ) FLA_malloc( n_threads * sizeof( FLA_Lock ) );
	args.hip_lock = hip_lock;

	for ( i = 0; i < n_threads; i++ )
		FLA_Lock_init( &(args.hip_lock[i]) );
#endif
	// Allocate and initialize HIP software cache.
	hip = ( FLA_Obj_hip* ) FLA_malloc( hip_n_blocks * n_threads * sizeof( FLA_Obj_hip ) );
	args.hip = hip;

	for ( i = 0; i < hip_n_blocks * n_threads; i++ )
	{
		args.hip[i].obj.base   = NULL;
		args.hip[i].buffer_hip = NULL;
		args.hip[i].clean      = TRUE;
		args.hip[i].request    = FALSE;

		//args.hip[i].obj.thread = 0;
	}

	victim = ( FLA_Obj_hip* ) FLA_malloc( n_threads * sizeof( FLA_Obj_hip ) );
	args.victim = victim;

	for ( i = 0; i < n_threads; i++ ){
		args.victim[i].obj.base = NULL;
		//args.victim[i].obj.thread = -1;
	}
	hip_log = ( FLA_Obj_hip* ) FLA_malloc( hip_n_blocks * n_threads * sizeof( FLA_Obj_hip ) );
	args.hip_log = hip_log;
#endif





	// Initialize tasks with critical information.
	FLASH_Queue_init_tasks( ( void* ) &args );


	//debug
/*
	FLASH_Task * temp_t = FLASH_Queue_get_head_task();
	FLASH_Dep * temp_d;
        for(int temp = 0; temp < n_tasks; temp++){
                if(temp_t == NULL) break;
                //if(temp_t->input_arg[0] == NULL) break;
                //if(temp_t->output_arg[0] == NULL) break;
                if(temp_t->n_dep_args == 0)
                {
                        printf("DPDC FLASH_Queue_init_tasks TASK %s %d dpdc is NULL \n", temp_t->name, temp_t->order);
                        temp_t = temp_t->next_task;
                        continue;
                }
                temp_d = temp_t->dep_arg_head;
                for(int i = 0; i < temp_t->n_dep_args; i++){

                        printf("DPDC FLASH_Queue_init_tasks TASK %s %d dpdc %d is %s %d \n", temp_t->name, temp_t->order, i, temp_d->task->name, temp_d->task->order );
                        temp_d = temp_d->next_dep;

                }


                for(int i = 0; i < temp_t->n_output_args; i++){
                        printf(" BEFORE FLASH_Queue_init_tasks TASK %s output args %d has thread %d\n", temp_t->name, i, temp_t->output_arg[i].base->thread);
                }
                temp_t = temp_t->next_task;

        }


*/





/*
	FLASH_Task * temp_t = FLASH_Queue_get_head_task();
	for(int temp = 0; temp < n_tasks; temp++){
		if(temp_t == NULL) break;
		//if(temp_t->input_arg[0] == NULL) break;
		//if(temp_t->output_arg[0] == NULL) break;
		//printf("INIT TASK %s \n", temp_t->name);
		for(int i = 0; i < temp_t->n_input_args; i++){
			printf("AFTER FLASH_Queue_init_tasks TASK %s input args %d has thread %c\n", temp_t->name, i, temp_t->input_arg[i].base->thread);
		}
		for(int i = 0; i < temp_t->n_output_args; i++){
			printf("AFTER FLASH_Queue_init_tasks TASK %s output args %d has thread %c\n", temp_t->name, i, temp_t->output_arg[i].base->thread);
		}
		printf("-------------------------------------------------------------------------------\n");


		temp_t = temp_t->next_task;
	}
*/




	// Display verbose output before free all tasks. 
	if ( FLASH_Queue_get_verbose_output() )
		FLASH_Queue_verbose_output();

	// Start timing the parallel execution.
	dtime = FLA_Clock();

#ifdef FLA_ENABLE_MULTITHREADING
	// Parallel Execution!
	FLASH_Queue_exec_parallel( ( void* ) &args );
#else
	// Simulation!
	FLASH_Queue_exec_simulation( ( void* ) &args );
#endif

	// End timing the parallel execution.
	dtime = FLA_Clock() - dtime;
	FLASH_Queue_set_parallel_time( dtime );

#ifdef FLA_ENABLE_MULTITHREADING   
	// Destroy the locks.
	FLA_Lock_destroy( &(args.all_lock) );

	for ( i = 0; i < n_queues; i++ )
	{
		FLA_Lock_destroy( &(args.run_lock[i]) );
	}

	for ( i = 0; i < n_threads; i++ )
	{
		FLA_Lock_destroy( &(args.dep_lock[i]) );
		FLA_Lock_destroy( &(args.war_lock[i]) );
	}

	for ( i = 0; i < n_caches; i++ )
	{
		FLA_Lock_destroy( &(args.cac_lock[i]) );
	}

	// Deallocate memory.
	FLA_free( run_lock );
	FLA_free( dep_lock );
	FLA_free( war_lock );
	FLA_free( cac_lock );
#endif

	FLA_free( cache );
	FLA_free( prefetch );
	FLA_free( wait_queue );

#ifdef FLA_ENABLE_GPU
#ifdef FLA_ENABLE_MULTITHREADING
	for ( i = 0; i < n_threads; i++ )
		FLA_Lock_destroy( &(args.gpu_lock[i]) );
	FLA_free( gpu_lock );
#endif
	FLA_free( gpu );
	FLA_free( victim );
	FLA_free( gpu_log );
#endif

#ifdef FLA_ENABLE_HIP
#ifdef FLA_ENABLE_MULTITHREADING
	for ( i = 0; i < n_threads; i++ )
		FLA_Lock_destroy( &(args.hip_lock[i]) );
	FLA_free( hip_lock );
#endif
	FLA_free( hip );
	FLA_free( victim );
	FLA_free( hip_log );
#endif

	// Reset values for next call to FLASH_Queue_exec().
	FLASH_Queue_reset();

	return;
}


void FLASH_Queue_init_tasks( void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_init_tasks

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int            i, j, k;
	int            n_tasks    = FLASH_Queue_get_num_tasks();
	int            n_queues   = args->n_queues;
	int            n_prefetch = 0;
	int            n_ready    = 0;
	int            length     = 0;
	int            width      = 0;
	int            height     = 0;
	int            size       = args->size;
	FLASH_Data_aff data_aff   = FLASH_Queue_get_data_affinity();
	FLASH_Task*    t;
	FLASH_Dep*     d;
	FLA_Obj        obj;

#ifdef FLA_ENABLE_GPU
	dim_t block_size      = 0;
	FLA_Datatype datatype = FLA_FLOAT;
	dim_t datatype_size   = FLA_Obj_datatype_size( datatype );
#endif

#ifdef FLA_ENABLE_HIP
	dim_t block_size      = 0;
	FLA_Datatype datatype = FLA_FLOAT;
	dim_t datatype_size   = FLA_Obj_datatype_size( datatype );
#endif

	// Find the 2D factorization of the number of threads.
	if ( data_aff == FLASH_QUEUE_AFFINITY_2D_BLOCK_CYCLIC )
	{
		int sq_rt = 0;
		while ( sq_rt * sq_rt <= n_queues ) sq_rt++;
		sq_rt--;
		while ( n_queues % sq_rt != 0 ) sq_rt--;
		length = n_queues / sq_rt;
		width  = sq_rt;     
	}

	// Grab the tail of the task queue.
	t = FLASH_Queue_get_tail_task();

	for ( i = n_tasks - 1; i >= 0; i-- )
	{
		// Determine data affinity.
		if ( data_aff == FLASH_QUEUE_AFFINITY_NONE )
		{ // No data affinity
			t->queue = 0;
		}
		else 
		{
			// Use the first output block to determine data affinity.
			obj = t->output_arg[0];

			// Use the top left block of the macroblock.
			if ( FLA_Obj_elemtype( obj ) == FLA_MATRIX )
				obj = *FLASH_OBJ_PTR_AT( obj );

			if ( data_aff == FLASH_QUEUE_AFFINITY_2D_BLOCK_CYCLIC )
			{ // Two-dimensional block cyclic           
				t->queue = ( obj.base->m_index % length ) +
					( obj.base->n_index % width  ) * length;
			}
			else if ( data_aff == FLASH_QUEUE_AFFINITY_1D_ROW_BLOCK_CYCLIC )
			{ // One-dimensional row block cyclic
				t->queue = obj.base->m_index % n_queues;
			}
			else if ( data_aff == FLASH_QUEUE_AFFINITY_1D_COLUMN_BLOCK_CYCLIC )
			{ // One-dimensional column block cyclic
				t->queue = obj.base->n_index % n_queues;
			}
			else
			{ // Round-robin
				t->queue = t->queue % n_queues;
			}
		}

		// Determine the height of each task in the DAG.
		height = 0;
		d = t->dep_arg_head;

		// Take the maximum height of dependent tasks.
		for ( j = 0; j < t->n_dep_args; j++ )
		{
			height = max( height, d->task->height );
			d = d->next_dep;
		}

		t->height = height + 1;

		// Since freeing a task is always a leaf, we want to force it to execute 
		// earlier by giving it a greater height in order to reclaim memory.
		if ( t->func == (void *) FLA_Obj_free_buffer_task )
			t->height += n_tasks;

#ifdef FLA_ENABLE_GPU
		for ( j = 0; j < t->n_output_args + t->n_input_args; j++ )
		{
			// Find the correct input or output argument.
			if ( j < t->n_output_args )
				obj = t->output_arg[j];
			else
				obj = t->input_arg[j - t->n_output_args];

			// Macroblock is used.
			if ( FLA_Obj_elemtype( obj ) == FLA_MATRIX )
			{
				dim_t    jj, kk;
				dim_t    m   = FLA_Obj_length( obj );
				dim_t    n   = FLA_Obj_width( obj );
				dim_t    cs  = FLA_Obj_col_stride( obj );
				FLA_Obj* buf = FLASH_OBJ_PTR_AT( obj );

				// Check each block in macroblock.
				for ( jj = 0; jj < n; jj++ )
				{
					for ( kk = 0; kk < m; kk++ )
					{
						obj = *( buf + jj * cs + kk );

						block_size = max( FLA_Obj_length( obj ) * FLA_Obj_width( obj ), block_size );

						if ( jj == 0 && FLA_Obj_datatype( obj ) != datatype && FLA_Obj_datatype_size( FLA_Obj_datatype( obj ) ) > datatype_size )
						{
							datatype      = FLA_Obj_datatype( obj );
							datatype_size = FLA_Obj_datatype_size( datatype );
						}
					}
				}
			}
			else // Regular block.
			{
				block_size = max( FLA_Obj_length( obj ) * FLA_Obj_width( obj ), block_size );

				if ( FLA_Obj_datatype( obj ) != datatype && FLA_Obj_datatype_size( FLA_Obj_datatype( obj ) ) > datatype_size )
				{
					datatype      = FLA_Obj_datatype( obj );
					datatype_size = FLA_Obj_datatype_size( datatype );
				}
			}
		}
#endif

#ifdef FLA_ENABLE_HIP
		for ( j = 0; j < t->n_output_args + t->n_input_args; j++ )
		{



			// Find the correct input or output argument.
			if ( j < t->n_output_args ){
				obj = t->output_arg[j];
				t->output_arg[j].base->thread = 0;
				t->output_arg[j].base->occupied = -1;
			}
			else{
				obj = t->input_arg[j - t->n_output_args];
				t->input_arg[j - t->n_output_args].base->thread = 0;
				t->input_arg[j - t->n_output_args].base->occupied = -1;
			}
			// Macroblock is used.
			if ( FLA_Obj_elemtype( obj ) == FLA_MATRIX )
			{
				dim_t    jj, kk;
				dim_t    m   = FLA_Obj_length( obj );
				dim_t    n   = FLA_Obj_width( obj );
				dim_t    cs  = FLA_Obj_col_stride( obj );
				FLA_Obj* buf = FLASH_OBJ_PTR_AT( obj );

				// Check each block in macroblock.
				for ( jj = 0; jj < n; jj++ )
				{
					for ( kk = 0; kk < m; kk++ )
					{
						obj = *( buf + jj * cs + kk );

						block_size = max( FLA_Obj_length( obj ) * FLA_Obj_width( obj ), block_size );

						if ( jj == 0 && FLA_Obj_datatype( obj ) != datatype && FLA_Obj_datatype_size( FLA_Obj_datatype( obj ) ) > datatype_size )
						{
							datatype      = FLA_Obj_datatype( obj );
							datatype_size = FLA_Obj_datatype_size( datatype );
						}
					}
				}
			}
			else // Regular block.
			{
				block_size = max( FLA_Obj_length( obj ) * FLA_Obj_width( obj ), block_size );

				if ( FLA_Obj_datatype( obj ) != datatype && FLA_Obj_datatype_size( FLA_Obj_datatype( obj ) ) > datatype_size )
				{
					datatype      = FLA_Obj_datatype( obj );
					datatype_size = FLA_Obj_datatype_size( datatype );
				}
			}
		}
#endif

		// Find the first blocks accessed each task.
		if ( n_prefetch < size )
		{
			for ( j = 0; j < t->n_output_args; j++ )
			{
				obj = t->output_arg[j];

				// Macroblock is used.
				if ( FLA_Obj_elemtype( obj ) == FLA_MATRIX )
				{
					dim_t    jj, kk;
					dim_t    m   = FLA_Obj_length( obj );
					dim_t    n   = FLA_Obj_width( obj );
					dim_t    cs  = FLA_Obj_col_stride( obj );
					FLA_Obj* buf = FLASH_OBJ_PTR_AT( obj );

					// Check each block in macroblock.
					for ( jj = 0; jj < n; jj++ )
					{
						for ( kk = 0; kk < m; kk++ )
						{
							obj = *( buf + jj * cs + kk );

							k = obj.base->n_write_blocks;

							// This block is one of the first blocks to be accessed.
							if ( k < size && k == n_prefetch )
							{
								args->prefetch[k] = obj;
								n_prefetch++;
							}
						}
					}
				}
				else // Regular block.
				{
					k = obj.base->n_write_blocks;

					// This block is one of the first blocks to be accessed.
					if ( k < size && k == n_prefetch )
					{
						args->prefetch[k] = obj;
						n_prefetch++;
					}
				}
			}
		}

		// Find all ready tasks.
		t->n_ready += t->n_input_args + t->n_output_args + 
			t->n_macro_args + t->n_war_args;

		if ( t->n_ready == 0 )
		{
			// Save the number of ready and available tasks.
			n_ready++;
		}

		// Go to the previous task.
		t = t->prev_task;
	}

	// Grab the head of the task queue.
	t = FLASH_Queue_get_head_task();

	for ( i = 0; i < n_tasks && n_ready > 0; i++ )
	{
		if ( t->n_ready == 0 )
		{
			// Enqueue all the ready and available tasks.
			FLASH_Queue_wait_enqueue( t, arg );

			// Decrement the number of ready tasks left to be enqueued.
			n_ready--;
		}

		// Go to the next task.
		t = t->next_task;
	}

#ifdef FLA_ENABLE_GPU
	args->block_size = block_size;
	args->datatype   = datatype;
#endif

#ifdef FLA_ENABLE_HIP
	args->block_size = block_size;
	args->datatype   = datatype;
#endif

	return;
}


void FLASH_Queue_wait_enqueue( FLASH_Task* t, void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_wait_enqueue

	  ----------------------------------------------------------------------------*/
{  
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int queue = t->queue;

	if ( args->wait_queue[queue].n_tasks == 0 )
	{
		args->wait_queue[queue].head = t;
		args->wait_queue[queue].tail = t;
	}
	else
	{
		t->prev_wait = args->wait_queue[queue].tail;

		// Insertion sort of tasks in waiting queue.
		if ( FLASH_Queue_get_sorting() )
		{
			while ( t->prev_wait != NULL )
			{
				if ( t->prev_wait->height >= t->height )
					break;

				t->next_wait = t->prev_wait;
				t->prev_wait = t->prev_wait->prev_wait;
			}         
		}

		// Checking if the task is the head of the waiting queue.      
		if ( t->prev_wait == NULL )
			args->wait_queue[queue].head = t;
		else
			t->prev_wait->next_wait = t;

		// Checking if the task is the tail of the waiting queue.
		if ( t->next_wait == NULL )
			args->wait_queue[queue].tail = t;
		else
			t->next_wait->prev_wait = t;
	}

	// Increment number of tasks on waiting queue.
	args->wait_queue[queue].n_tasks++;

	return;
}


FLASH_Task* FLASH_Queue_wait_dequeue( int queue, int cache, void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_wait_dequeue

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	FLASH_Task* t = NULL;
	FLA_Bool enabled = FALSE;

#ifdef FLA_ENABLE_GPU
	enabled = FLASH_Queue_get_enabled_gpu();
#endif

#ifdef FLA_ENABLE_HIP
	enabled = FLASH_Queue_get_enabled_hip();
#endif

	if ( args->wait_queue[queue].n_tasks > 0 )
	{
		// Dequeue the first task.
		t = args->wait_queue[queue].head;

		if ( args->wait_queue[queue].n_tasks == 1 )
		{
			// Clear the queue of its only task.
			args->wait_queue[queue].head = NULL;
			args->wait_queue[queue].tail = NULL;        
		}
		else
		{
			// Grab a new task if using cache affinity.
			if ( FLASH_Queue_get_caching() )
			{
				// Determine if using GPU or not.
				if ( enabled )
				{
#ifdef FLA_ENABLE_GPU
#ifdef FLA_ENABLE_MULTITHREADING
					FLA_Lock_acquire( &(args->gpu_lock[cache]) ); // G ***      
#endif
					// Find a task where the task has blocks currently in GPU.
					t = FLASH_Queue_wait_dequeue_block( queue, cache, arg );

#ifdef FLA_ENABLE_MULTITHREADING      
					FLA_Lock_release( &(args->gpu_lock[cache]) ); // G ***
#endif
#endif
#ifdef FLA_ENABLE_HIP
#ifdef FLA_ENABLE_MULTITHREADING
					FLA_Lock_acquire( &(args->hip_lock[cache]) ); // G ***      
#endif
					// Find a task where the task has blocks currently on a HIP device.
					t = FLASH_Queue_wait_dequeue_block( queue, cache, arg );

#ifdef FLA_ENABLE_MULTITHREADING      
					FLA_Lock_release( &(args->hip_lock[cache]) ); // G ***
#endif
#endif
				}
				else
				{
#ifdef FLA_ENABLE_MULTITHREADING
					FLA_Lock_acquire( &(args->cac_lock[cache]) ); // C ***      
#endif
					// Find a task where the task has blocks currently in cache.
					t = FLASH_Queue_wait_dequeue_block( queue, cache, arg );

#ifdef FLA_ENABLE_MULTITHREADING      
					FLA_Lock_release( &(args->cac_lock[cache]) ); // C ***
#endif
				}

				// Adjust pointers if the task is head of waiting queue.
				if ( t->prev_wait == NULL )
				{
					args->wait_queue[queue].head = t->next_wait;
					args->wait_queue[queue].head->prev_wait = NULL;
				}
				else
				{
					t->prev_wait->next_wait = t->next_wait;
				}

				// Adjust pointers if the task is tail of waiting queue.
				if ( t->next_wait == NULL )
				{
					args->wait_queue[queue].tail = t->prev_wait;
					args->wait_queue[queue].tail->next_wait = NULL;
				}
				else
				{
					t->next_wait->prev_wait = t->prev_wait;
				}
			}
			else
			{
				// Adjust pointers in waiting queue.
				args->wait_queue[queue].head = t->next_wait;
				args->wait_queue[queue].head->prev_wait = NULL;
			}
		}

		// Clear the task's waiting linked list pointers.
		t->prev_wait = NULL;
		t->next_wait = NULL;

		// Decrement number of tasks on waiting queue.
		args->wait_queue[queue].n_tasks--;     
	}
	//if(t != NULL)
	//printf("DEQUEUE TASK %s t->input thread %d  t->output thread %d\n", t->name, t->input_arg[0].thread, t->output_arg[0].thread );

	return t;
}


FLASH_Task* FLASH_Queue_wait_dequeue_block( int queue, int cache, void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_wait_dequeue_block

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int         i, j, k;
	int         size    = args->size;
	int         n_tasks = args->wait_queue[queue].n_tasks;
	FLA_Bool    enabled = FALSE;
	FLASH_Task* t;
	FLA_Obj     obj;
	FLA_Obj     mem;

#ifdef FLA_ENABLE_GPU
	enabled = FLASH_Queue_get_enabled_gpu();

	// If using GPUs, then only check GPU and not the cache.
	if ( enabled )
		size = FLASH_Queue_get_gpu_num_blocks();
#endif

#ifdef FLA_ENABLE_HIP
	enabled = FLASH_Queue_get_enabled_hip();

	// If using HIP, then only check HIP and not the cache.
	if ( enabled )
		size = FLASH_Queue_get_hip_num_blocks();
#endif

	t = args->wait_queue[queue].head;

	// Check if any of the output blocks are in the cache.
	for ( i = 0; i < n_tasks; i++ )
	{
		for ( j = 0; j < size; j++ )
		{
			// Initialize the memory just in case.
			mem.base = NULL;

			// Determine if using GPU or not.
			if ( enabled )
			{
#ifdef FLA_ENABLE_GPU
				mem = args->gpu[cache * size + j].obj;
#endif
#ifdef FLA_ENABLE_HIP
				mem = args->hip[cache * size + j].obj;
#endif
			}
			else
			{
				mem = args->cache[cache * size + j];
			}

			for ( k = 0; k < t->n_output_args; k++ )
			{	
				obj = t->output_arg[k];

				if ( FLA_Obj_elemtype( obj ) == FLA_MATRIX )
					obj = *FLASH_OBJ_PTR_AT( obj );

				// Return the task if its output block is in cache.
				if ( mem.base == obj.base )
				{
					t->hit = TRUE;
					return t;
				}
			}
		}
		t = t->next_wait;
	}

	return args->wait_queue[queue].head;
}


void FLASH_Queue_update_cache( FLASH_Task* t, void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_update_cache

	  ----------------------------------------------------------------------------*/
{
	int      i, j;
	FLA_Bool duplicate;
	FLA_Obj  obj;

	if ( t == NULL )
		return;

	// Updating the input blocks.
	for ( i = t->n_input_args - 1; i >= 0; i-- )
	{
		// Check for duplicate blocks.
		duplicate = FALSE;

		for ( j = 0; j < t->n_output_args && !duplicate; j++ )
		{
			if ( t->input_arg[i].base == t->output_arg[j].base )
				duplicate = TRUE;
		}

		for ( j = 0; j < i && !duplicate; j++ )
		{
			if ( t->input_arg[i].base == t->input_arg[j].base )
				duplicate = TRUE;
		}

		// If the input block has not been processed before.
		if ( !duplicate )
		{
			obj = t->input_arg[i];

			// Macroblock is used.
			if ( FLA_Obj_elemtype( obj ) == FLA_MATRIX )
			{
				dim_t    jj, kk;
				dim_t    m    = FLA_Obj_length( obj );
				dim_t    n    = FLA_Obj_width( obj );
				dim_t    cs   = FLA_Obj_col_stride( obj );
				FLA_Obj* buf  = FLASH_OBJ_PTR_AT( obj );

				// Dependence analysis for each input block in macroblock.
				for ( jj = 0; jj < n; jj++ )
					for ( kk = 0; kk < m; kk++ )
						FLASH_Queue_update_cache_block( *( buf + jj * cs + kk ), 
								t->cache, FALSE, arg );
			}
			else // Regular block.
			{
				FLASH_Queue_update_cache_block( obj, t->cache, FALSE, arg );
			}
		}
	}

	// Updating the output blocks.
	for ( i = t->n_output_args - 1; i >= 0; i-- )
	{
		// Check for duplicate blocks.
		duplicate = FALSE;

		for ( j = 0; j < i && !duplicate; j++ )
		{
			if ( t->output_arg[i].base == t->output_arg[j].base )
				duplicate = TRUE;
		}

		// If the output block has not been processed before.
		if ( !duplicate )
		{
			obj = t->output_arg[i];

			// Macroblock is used.
			if ( FLA_Obj_elemtype( obj ) == FLA_MATRIX )
			{
				dim_t    jj, kk;
				dim_t    m    = FLA_Obj_length( obj );
				dim_t    n    = FLA_Obj_width( obj );
				dim_t    cs   = FLA_Obj_col_stride( obj );
				FLA_Obj* buf  = FLASH_OBJ_PTR_AT( obj );

				// Dependence analysis for each input block in macroblock.
				for ( jj = 0; jj < n; jj++ )
					for ( kk = 0; kk < m; kk++ )
						FLASH_Queue_update_cache_block( *( buf + jj * cs + kk ),
								t->cache, TRUE, arg );
			}
			else // Regular block.
			{
				FLASH_Queue_update_cache_block( obj, t->cache, TRUE, arg );
			}
		}
	}   

	return;
}


void FLASH_Queue_update_cache_block( FLA_Obj obj,
		int cache, 
		FLA_Bool output,
		void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_update_cache_block

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int i, j, k;
	int n_caches = args->n_caches;
	int size     = args->size;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->cac_lock[cache]) ); // C ***      
#endif

	// Locate the position of the block in the cache.
	for ( k = 0; k < size - 1; k++ )
	{
		if ( obj.base == args->cache[cache * size + k].base )
			break;
	}

	// Shift all the previous tasks for LRU replacement.
	for ( j = k; j > 0; j-- )
		args->cache[cache * size + j] = args->cache[cache * size + j - 1];

	// Place the block on the cache as the most recently used.
	args->cache[cache * size] = obj;

#ifdef FLA_ENABLE_MULTITHREADING      
	FLA_Lock_release( &(args->cac_lock[cache]) ); // C ***
#endif

	// Write invalidate if updating with output block.
	if ( output )
	{
		for ( i = 0; i < n_caches; i++ )
		{
			if ( i != cache )
			{
#ifdef FLA_ENABLE_MULTITHREADING
				FLA_Lock_acquire( &(args->cac_lock[i]) ); // C ***      
#endif
				// Locate the position of the block in the cache.
				for ( k = 0; k < size; k++ )
				{
					if ( obj.base == args->cache[i * size + k].base )
						break;
				}

				// The block is owned by other thread.
				if ( k < size )
				{
					// Shift all the blocks for the invalidated block.
					for ( j = k; j < size - 1; j++ )
						args->cache[i * size + j] = args->cache[i * size + j + 1];

					// Invalidate the block.
					args->cache[i * size + size - 1].base = NULL;
				}
#ifdef FLA_ENABLE_MULTITHREADING      
				FLA_Lock_release( &(args->cac_lock[i]) ); // C ***
#endif
			}
		}
	}

	return;
}


void FLASH_Queue_prefetch( int cache, void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_prefetch

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;   
	int i;
	int size = args->size;
	FLA_Obj obj;

	// Prefetch blocks in opposite order to maintain LRU.
	for ( i = size - 1; i >= 0; i-- )
	{
		obj = args->prefetch[i];

		// Only prefetch if it is a valid block.
		if ( obj.base != NULL )
		{
			// Prefetch the block.
			FLASH_Queue_prefetch_block( obj );

			// Record the prefetched block in the cache.
			args->cache[cache * size + i] = obj;
		}
	}

	return;
}


void FLASH_Queue_prefetch_block( FLA_Obj obj )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_prefetch_block

	  ----------------------------------------------------------------------------*/
{
	int          i, inc;
	int          line_size = FLASH_Queue_get_cache_line_size();
	int          elem_size = FLA_Obj_elem_size( obj );
	int          length    = FLA_Obj_length( obj );
	int          width     = FLA_Obj_width( obj );
	FLA_Datatype datatype  = FLA_Obj_datatype( obj );

	// Determine stride to prefetch block into cache.
	inc = line_size / elem_size;

	// Switch between the four different datatypes.
	switch ( datatype )
	{
		case FLA_FLOAT:
			{
				float *buffer = ( float * ) FLA_FLOAT_PTR( obj );
				float access;

				// Access each cache line of the block.
				for ( i = 0; i < length * width; i += inc )
					access = buffer[i];

				// Prevent dead code elimination.
				access += 1.0;

				break;
			}
		case FLA_DOUBLE:
			{
				double *buffer = ( double * ) FLA_DOUBLE_PTR( obj );
				double access;

				// Access each cache line of the block.
				for ( i = 0; i < length * width; i += inc )
					access = buffer[i];

				// Prevent dead code elimination.
				access += 1.0;

				break;
			}
		case FLA_COMPLEX:
			{
				scomplex *buffer = ( scomplex * ) FLA_COMPLEX_PTR( obj );
				scomplex access;

				// Access each cache line of the block.
				for ( i = 0; i < length * width; i += inc )
					access = buffer[i];

				// Prevent dead code elimination.
				access.real += 1.0;

				break;
			}
		case FLA_DOUBLE_COMPLEX:
			{
				dcomplex *buffer = ( dcomplex * ) FLA_DOUBLE_COMPLEX_PTR( obj );
				dcomplex access;

				// Access each cache line of the block.
				for ( i = 0; i < length * width; i += inc )
					access = buffer[i];

				// Prevent dead code elimination.
				access.real += 1.0;

				break;
			}
		case FLA_INT:
			{
				int *buffer = ( int * ) FLA_INT_PTR( obj );
				int access;

				// Access each cache line of the block.
				for ( i = 0; i < length * width; i += inc )
					access = buffer[i];

				// Prevent dead code elimination.
				access += 1.0;

				break;
			}
		default:
			// This default case should never execute.
			FLA_Check_error_code( FLA_INVALID_DATATYPE );
	}

	return;
}


FLASH_Task* FLASH_Queue_work_stealing( int queue, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_work_stealing

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int         q;
	int         n_queues = args->n_queues;
	FLASH_Task* t = NULL;

	// Do not perform work stealing if there is only one queue.
	if ( n_queues == 1 )
		return t;

	// Find a random queue not equal to the current queue.
	do
	{
#ifdef FLA_ENABLE_WINDOWS_BUILD
		rand_s( &q );
		q = q % n_queues;
#else
#ifdef FLA_ENABLE_TIDSP
		q = rand() % n_queues;
#else
		q = lrand48() % n_queues;
#endif
#endif
	}
	while ( q == queue );

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->run_lock[q]) ); // R ***
#endif

	// If there are tasks that this thread can steal.
	if ( args->wait_queue[q].n_tasks > 0 )
	{
		// Dequeue the last task.
		t = args->wait_queue[q].tail;

		if ( args->wait_queue[q].n_tasks == 1 )
		{
			// Clear the queue of its only task.
			args->wait_queue[q].head = NULL;
			args->wait_queue[q].tail = NULL;
		}
		else
		{
			// Adjust pointers in waiting queue.
			args->wait_queue[q].tail = t->prev_wait;
			args->wait_queue[q].tail->next_wait = NULL;
		}

		// Reset waiting queue data about the stolen task.
		t->queue = queue;
		t->prev_wait = NULL;
		t->next_wait = NULL;

		args->wait_queue[q].n_tasks--;
	}

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->run_lock[q]) ); // R ***
#endif

	return t;
}

#ifdef FLA_ENABLE_GPU

void FLASH_Queue_create_gpu( int thread, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_create_gpu

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int i;
	dim_t gpu_n_blocks     = FLASH_Queue_get_gpu_num_blocks();
	dim_t block_size       = args->block_size;
	FLA_Datatype datatype  = args->datatype;

	// Exit if not using GPU.
	if ( !FLASH_Queue_get_enabled_gpu() )
		return;

	// Bind thread to GPU.
	FLASH_Queue_bind_gpu( thread );

	// Allocate the memory on the GPU for all the blocks a priori.
	for ( i = 0; i < gpu_n_blocks; i++ )
		FLASH_Queue_alloc_gpu( block_size, datatype, &(args->gpu[thread * gpu_n_blocks + i].buffer_gpu) );

	return;
}


void FLASH_Queue_destroy_gpu( int thread, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_destroy_gpu

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int i;
	dim_t gpu_n_blocks = FLASH_Queue_get_gpu_num_blocks();
	FLA_Obj_gpu gpu_obj;

	// Exit if not using GPU.
	if ( !FLASH_Queue_get_enabled_gpu() )
		return;

	// Examine every block left on the GPU.
	for ( i = 0; i < gpu_n_blocks; i++ )
	{
		gpu_obj = args->gpu[thread * gpu_n_blocks + i];

		// Flush the blocks that are dirty.
		if ( gpu_obj.obj.base != NULL && !gpu_obj.clean )
			FLASH_Queue_read_gpu( gpu_obj.obj, gpu_obj.buffer_gpu );

		// Free the memory on the GPU for all the blocks.
		FLASH_Queue_free_gpu( gpu_obj.buffer_gpu );
	}

	return;
}


FLA_Bool FLASH_Queue_exec_gpu( FLASH_Task *t, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_exec_gpu

	  ----------------------------------------------------------------------------*/
{
	void** input_arg;
	void** output_arg;

	if ( t == NULL )
		return TRUE;

	// If not using the GPU, then execute on CPU.
	if ( !FLASH_Queue_get_enabled_gpu() )
	{
		FLASH_Queue_exec_task( t );

		return TRUE;
	}

	// Check if all the operands are ready and up to date.
	if ( !FLASH_Queue_check_gpu( t, arg ) )
	{
		FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
		int queue = t->queue;
		t->hit = FALSE;

#ifdef FLA_ENABLE_MULTITHREADING
		FLA_Lock_acquire( &(args->run_lock[queue]) ); // R ***
#endif
		// Reenqueue the task if the blocks are not all flushed.
		FLASH_Queue_wait_enqueue( t, arg );

#ifdef FLA_ENABLE_MULTITHREADING
		FLA_Lock_release( &(args->run_lock[queue]) ); // R ***    
#endif

		return FALSE;
	}

	// If GPU is enabled, but the task is not supported for GPU execution.
	if ( !t->enabled_gpu )
	{
		int i, j, k;
		int thread        = t->thread;
		int n_input_args  = t->n_input_args;
		int n_output_args = t->n_output_args;
		int n_threads     = FLASH_Queue_get_num_threads();
		FLA_Bool duplicate;
		FLA_Obj  obj;

		// Check the blocks on each GPU.
		for ( k = 0; k < n_threads; k++ )
		{
			// Check the input and output arguments on the GPUs.
			for ( i = 0; i < n_input_args + n_output_args; i++ )
			{
				// Check for duplicate blocks.
				duplicate = FALSE;

				// Find the correct input or output argument.
				if ( i < n_input_args )
				{
					obj = t->input_arg[i];

					for ( j = 0; j < n_output_args && !duplicate; j++ )
					{
						if ( obj.base == t->output_arg[j].base )
							duplicate = TRUE;
					}

					for ( j = 0; j < i && !duplicate; j++ )
					{
						if ( obj.base == t->input_arg[j].base )
							duplicate = TRUE;
					}
				}
				else
				{
					obj = t->output_arg[i - n_input_args];

					for ( j = 0; j < i - n_input_args && !duplicate; j++ )
					{
						if ( obj.base == t->output_arg[j].base )
							duplicate = TRUE;
					}        
				}

				// If the block has not been processed before.
				if ( !duplicate )
				{     
					// Macroblock is used.
					if ( FLA_Obj_elemtype( obj ) == FLA_MATRIX )
					{
						dim_t    jj, kk;
						dim_t    m    = FLA_Obj_length( obj );
						dim_t    n    = FLA_Obj_width( obj );
						dim_t    cs   = FLA_Obj_col_stride( obj );
						FLA_Obj* buf  = FLASH_OBJ_PTR_AT( obj );

						// Clear each block in macroblock.
						for ( jj = 0; jj < n; jj++ )
						{
							for ( kk = 0; kk < m; kk++ )
							{
								obj = *( buf + jj * cs + kk );

								// Flush the block to main memory if it is on the GPU.
								if ( k == thread )
									FLASH_Queue_flush_block_gpu( obj, k, arg );

								// Invalidate output block on all GPUs.
								if ( i >= n_input_args )
									FLASH_Queue_invalidate_block_gpu( obj, k, arg );
							}
						}
					}
					else
					{
						// Flush the block to main memory if it is on the GPU.
						if ( k == thread )
							FLASH_Queue_flush_block_gpu( obj, k, arg );

						// Invalidate output block on all GPUs.
						if ( i >= n_input_args )
							FLASH_Queue_invalidate_block_gpu( obj, k, arg );
					}
				}
			}
		}

		// Execute the task on CPU instead of GPU.
		FLASH_Queue_exec_task( t );

		return TRUE;
	}

	// Gather the pointers for the data on the GPU.
	input_arg = ( void** ) FLA_malloc( t->n_input_args * sizeof( void* ) );
	output_arg = ( void** ) FLA_malloc( t->n_output_args * sizeof( void* ) );

	// Bring all the blocks to GPU.
	FLASH_Queue_update_gpu( t, input_arg, output_arg, arg );

	// Execute the task on GPU.
	FLASH_Queue_exec_task_gpu( t, input_arg, output_arg );

	// Mark all the output blocks as dirty.
	FLASH_Queue_mark_gpu( t, arg );

	// Free memory.
	FLA_free( input_arg );
	FLA_free( output_arg );

	return TRUE;
}


FLA_Bool FLASH_Queue_check_gpu( FLASH_Task *t, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_check_gpu

	  ----------------------------------------------------------------------------*/
{
	int i, j, k;
	int thread        = t->thread;
	int n_input_args  = t->n_input_args;
	int n_output_args = t->n_output_args;
	int n_threads     = FLASH_Queue_get_num_threads();
	FLA_Bool r_val    = TRUE;
	FLA_Bool t_val;
	FLA_Bool duplicate;
	FLA_Obj  obj;

	// Check the input and output arguments on the GPUs.
	for ( i = 0; i < n_input_args + n_output_args; i++ )
	{
		// Check for duplicate blocks.
		duplicate = FALSE;

		// Find the correct input or output argument.
		if ( i < n_input_args )
		{
			obj = t->input_arg[i];

			for ( j = 0; j < n_output_args && !duplicate; j++ )
			{
				if ( obj.base == t->output_arg[j].base )
					duplicate = TRUE;
			}

			for ( j = 0; j < i && !duplicate; j++ )
			{
				if ( obj.base == t->input_arg[j].base )
					duplicate = TRUE;
			}
		}
		else
		{
			obj = t->output_arg[i - n_input_args];

			for ( j = 0; j < i - n_input_args && !duplicate; j++ )
			{
				if ( obj.base == t->output_arg[j].base )
					duplicate = TRUE;
			}        
		}

		// If the block has not been processed before.
		if ( !duplicate )
		{     
			// Macroblock is used.
			if ( FLA_Obj_elemtype( obj ) == FLA_MATRIX )
			{
				dim_t    jj, kk;
				dim_t    m    = FLA_Obj_length( obj );
				dim_t    n    = FLA_Obj_width( obj );
				dim_t    cs   = FLA_Obj_col_stride( obj );
				FLA_Obj* buf  = FLASH_OBJ_PTR_AT( obj );

				// Clear each block in macroblock.
				for ( jj = 0; jj < n; jj++ )
				{
					for ( kk = 0; kk < m; kk++ )
					{
						obj = *( buf + jj * cs + kk );

						t_val = TRUE;

						// Check to see if the block is dirty on another GPU.
						for ( k = 0; k < n_threads && t_val; k++ )
							if ( k != thread )
								t_val = t_val && FLASH_Queue_check_block_gpu( obj, k, arg );

						r_val = r_val && t_val;
					}
				}
			}
			else
			{
				t_val = TRUE;

				// Check to see if the block is dirty on another GPU.
				for ( k = 0; k < n_threads && t_val; k++ )
					if ( k != thread )
						t_val = t_val && FLASH_Queue_check_block_gpu( obj, k, arg );

				r_val = r_val && t_val;
			}
		}
	}

	return r_val;
}


FLA_Bool FLASH_Queue_check_block_gpu( FLA_Obj obj, int thread, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_check_block_gpu

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int k;
	dim_t gpu_n_blocks = FLASH_Queue_get_gpu_num_blocks();
	FLA_Bool r_val = TRUE;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->gpu_lock[thread]) ); // G ***
#endif

	// Locate the position of the block on the GPU.
	for ( k = 0; k < gpu_n_blocks; k++ )
		if ( obj.base == args->gpu[thread * gpu_n_blocks + k].obj.base )
			break;

	if ( k < gpu_n_blocks )
	{
		// Request this block if it is dirty.
		if ( !args->gpu[thread * gpu_n_blocks + k].clean )
		{
			args->gpu[thread * gpu_n_blocks + k].request = TRUE;

			r_val = FALSE;
		}
	}

	// Check the victim block.
	if ( obj.base == args->victim[thread].obj.base )
		r_val = FALSE;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->gpu_lock[thread]) ); // G ***
#endif

	return r_val;
}


void FLASH_Queue_update_gpu( FLASH_Task *t,
		void **input_arg,
		void **output_arg,
		void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_update_gpu

	  ----------------------------------------------------------------------------*/
{
	int i, j, k;
	int thread    = t->thread;
	int n_threads = FLASH_Queue_get_num_threads();
	FLA_Bool duplicate;

	// None of the arguments can be macroblocks yet.
	// Complicating factor is copying macroblock to contiguous memory on GPU.

	// Bring the input arguments to the GPU.
	for ( i = t->n_input_args - 1; i >= 0; i-- )
	{
		// Check for duplicate blocks.
		duplicate = FALSE;

		for ( j = 0; j < t->n_output_args && !duplicate; j++ )
		{
			if ( t->input_arg[i].base == t->output_arg[j].base )
				duplicate = TRUE;
		}

		for ( j = 0; j < i && !duplicate; j++ )
		{
			if ( t->input_arg[i].base == t->input_arg[j].base )
				duplicate = TRUE;
		}

		// If the input block has not been processed before.
		if ( !duplicate )
		{
			FLASH_Queue_update_block_gpu( t->input_arg[i], input_arg + i, thread, arg );
		}
		else
		{
			input_arg[i] = NULL;
		}
	}

	// Bring the output arguments to the GPU.
	for ( i = t->n_output_args - 1; i >= 0; i-- )
	{
		// Check for duplicate blocks.
		duplicate = FALSE;

		for ( j = 0; j < i && !duplicate; j++ )
		{
			if ( t->output_arg[i].base == t->output_arg[j].base )
				duplicate = TRUE;
		}

		// If the output block has not been processed before.
		if ( !duplicate )
		{
			FLASH_Queue_update_block_gpu( t->output_arg[i], output_arg + i, thread, arg );                                       

			// Invalidate output blocks on all other GPUs.
			for ( k = 0; k < n_threads; k++ )
				if ( k != thread )
					FLASH_Queue_invalidate_block_gpu( t->output_arg[i], k, arg );
		}
		else
		{
			output_arg[i] = NULL;
		}
	}

	// Check to see if there are any duplicates.
	for ( i = t->n_input_args - 1; i >= 0; i-- )
	{
		for ( j = 0; j < t->n_output_args && input_arg[i] == NULL; j++ )
		{
			if ( t->input_arg[i].base == t->output_arg[j].base )
				input_arg[i] = output_arg[j];
		}

		for ( j = 0; j < i && input_arg[i] == NULL; j++ )
		{
			if ( t->input_arg[i].base == t->input_arg[j].base )
				input_arg[i] = input_arg[j];
		}
	}

	// Check to see if there are any duplicates.
	for ( i = t->n_output_args - 1; i >= 0; i-- )
	{
		for ( j = 0; j < i && output_arg[i] == NULL; j++ )
		{
			if ( t->output_arg[i].base == t->output_arg[j].base )
				output_arg[i] = output_arg[j];
		}
	}

	return;
}


void FLASH_Queue_update_block_gpu( FLA_Obj obj,
		void **buffer_gpu,
		int thread, 
		void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_update_block_gpu

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int j, k;
	dim_t gpu_n_blocks = FLASH_Queue_get_gpu_num_blocks();
	FLA_Bool transfer = FALSE;
	FLA_Bool evict = FALSE;
	FLA_Obj_gpu evict_obj;   
	FLA_Obj_gpu gpu_obj;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->gpu_lock[thread]) ); // G ***
#endif

	// Locate the position of the block on GPU.
	for ( k = 0; k < gpu_n_blocks - 1; k++ )
		if ( obj.base == args->gpu[thread * gpu_n_blocks + k].obj.base )
			break;

	// Save the pointer to the data on the GPU.
	buffer_gpu[0] = args->gpu[thread * gpu_n_blocks + k].buffer_gpu;

	// Save the victim block.
	evict_obj = args->gpu[thread * gpu_n_blocks + k];

	// The block is not already in the GPU.
	if ( obj.base != args->gpu[thread * gpu_n_blocks + k].obj.base )
	{
		// Save for data transfer outside of critical section.
		transfer = TRUE;

		// Save for eviction outside of critical section.
		if ( evict_obj.obj.base != NULL && !evict_obj.clean )
		{
			evict = TRUE;
			args->victim[thread] = evict_obj;
		}      

		// Save the block in the data structure.
		args->gpu[thread * gpu_n_blocks + k].obj = obj;

		// Make sure the new block is clean.
		args->gpu[thread * gpu_n_blocks + k].clean   = TRUE;
		args->gpu[thread * gpu_n_blocks + k].request = FALSE;
	}       

	// Use the block on the GPU that is a hit or LRU.
	gpu_obj = args->gpu[thread * gpu_n_blocks + k];

	// Shift all the previous tasks for LRU replacement.
	for ( j = k; j > 0; j-- )
		args->gpu[thread * gpu_n_blocks + j] = args->gpu[thread * gpu_n_blocks + j - 1];

	// Place the block on the cache as the most recently used.
	args->gpu[thread * gpu_n_blocks] = gpu_obj;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->gpu_lock[thread]) ); // G ***
#endif

	// Evict and flush the LRU dirty block.
	if ( evict )
	{
		FLASH_Queue_read_gpu( evict_obj.obj, evict_obj.buffer_gpu );

#ifdef FLA_ENABLE_MULTITHREADING
		FLA_Lock_acquire( &(args->gpu_lock[thread]) ); // G ***
#endif

		args->victim[thread].obj.base = NULL;

#ifdef FLA_ENABLE_MULTITHREADING
		FLA_Lock_release( &(args->gpu_lock[thread]) ); // G ***
#endif
	}

	// Move the block to the GPU.
	if ( transfer )
		FLASH_Queue_write_gpu( gpu_obj.obj, gpu_obj.buffer_gpu );

	return;
}


void FLASH_Queue_mark_gpu( FLASH_Task *t, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_mark_gpu

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int i, j, k;
	int thread = t->thread;
	dim_t gpu_n_blocks = FLASH_Queue_get_gpu_num_blocks();
	FLA_Bool duplicate;
	FLA_Obj  obj;

	// Mark all the output blocks on the GPU as dirty.
	for ( i = t->n_output_args - 1; i >= 0; i-- )
	{
		obj = t->output_arg[i];

		// Check for duplicate blocks.
		duplicate = FALSE;

		for ( j = 0; j < i && !duplicate; j++ )
		{
			if ( obj.base == t->output_arg[j].base )
				duplicate = TRUE;
		}

		// If the output block has not been processed before.
		if ( !duplicate )
		{
#ifdef FLA_ENABLE_MULTITHREADING
			FLA_Lock_acquire( &(args->gpu_lock[thread]) ); // G ***
#endif

			// Locate the position of the block on the GPU.
			for ( k = 0; k < gpu_n_blocks; k++ )
				if ( obj.base == args->gpu[thread * gpu_n_blocks + k].obj.base )
					break;

			if ( k < gpu_n_blocks )
			{
				// Change the bits for the new dirty block.
				args->gpu[thread * gpu_n_blocks + k].clean   = FALSE;
				args->gpu[thread * gpu_n_blocks + k].request = FALSE;
			}

#ifdef FLA_ENABLE_MULTITHREADING
			FLA_Lock_release( &(args->gpu_lock[thread]) ); // G ***
#endif
		}
	}

	return;
}


void FLASH_Queue_invalidate_block_gpu( FLA_Obj obj, int thread, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_invalidate_block_gpu

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int j, k;
	dim_t gpu_n_blocks = FLASH_Queue_get_gpu_num_blocks();
	FLA_Obj_gpu gpu_obj;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->gpu_lock[thread]) ); // G ***
#endif

	// Locate the position of the block on the GPU.
	for ( k = 0; k < gpu_n_blocks; k++ )
		if ( obj.base == args->gpu[thread * gpu_n_blocks + k].obj.base )
			break;

	// The block is owned by other GPU.
	if ( k < gpu_n_blocks )
	{
		// Invalidate the block.
		args->gpu[thread * gpu_n_blocks + k].obj.base = NULL;

		args->gpu[thread * gpu_n_blocks + k].clean    = TRUE;
		args->gpu[thread * gpu_n_blocks + k].request  = FALSE;

		// Save the block that will be invalidated.
		gpu_obj = args->gpu[thread * gpu_n_blocks + k];

		// Shift all the blocks for the invalidated block.
		for ( j = k; j < gpu_n_blocks - 1; j++ )
			args->gpu[thread * gpu_n_blocks + j] = args->gpu[thread * gpu_n_blocks + j + 1];

		// Move to the LRU block.
		args->gpu[thread * gpu_n_blocks + gpu_n_blocks - 1] = gpu_obj;
	}

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->gpu_lock[thread]) ); // G ***
#endif

	return;
}


void FLASH_Queue_flush_block_gpu( FLA_Obj obj, int thread, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_flush_block_gpu

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int k;
	dim_t gpu_n_blocks = FLASH_Queue_get_gpu_num_blocks();
	FLA_Bool transfer = FALSE;
	FLA_Obj_gpu gpu_obj;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->gpu_lock[thread]) ); // G ***
#endif

	// Locate the position of the block on the GPU.
	for ( k = 0; k < gpu_n_blocks; k++ )
		if ( obj.base == args->gpu[thread * gpu_n_blocks + k].obj.base )
			break;

	// The block is owned by the GPU.
	if ( k < gpu_n_blocks )
	{
		// Save the block that will be flushed.
		gpu_obj = args->gpu[thread * gpu_n_blocks + k];

		// If the block is dirty, then flush it.
		if ( gpu_obj.obj.base != NULL && !gpu_obj.clean )
			transfer = TRUE;
	}

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->gpu_lock[thread]) ); // G ***
#endif

	// Exit early if a flush is not required.
	if ( !transfer )
		return;

	// Flush the block outside the critical section.
	FLASH_Queue_read_gpu( gpu_obj.obj, gpu_obj.buffer_gpu );   

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->gpu_lock[thread]) ); // G ***
#endif

	// Locate the position of the block on the GPU.
	for ( k = 0; k < gpu_n_blocks; k++ )
		if ( obj.base == args->gpu[thread * gpu_n_blocks + k].obj.base )
			break;

	if ( k < gpu_n_blocks )
	{
		// Update the bits for the flushed block.
		args->gpu[thread * gpu_n_blocks + k].clean   = TRUE;
		args->gpu[thread * gpu_n_blocks + k].request = FALSE;
	}

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->gpu_lock[thread]) ); // G ***
#endif

	return;
}


void FLASH_Queue_flush_gpu( int thread, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_flush_gpu

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int i, k;
	dim_t gpu_n_blocks = FLASH_Queue_get_gpu_num_blocks();
	int n_transfer = 0;
	FLA_Obj_gpu gpu_obj;

	// Exit if not using GPU.
	if ( !FLASH_Queue_get_enabled_gpu() )
		return;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->gpu_lock[thread]) ); // G ***
#endif

	for ( k = 0; k < gpu_n_blocks; k++ )
	{
		// Save the block that might be flushed.
		gpu_obj = args->gpu[thread * gpu_n_blocks + k];

		// Flush the block if it is dirty and requested.
		if ( gpu_obj.obj.base != NULL && !gpu_obj.clean && gpu_obj.request )
		{
			// Save the block for data transfer outside the critical section.
			args->gpu_log[thread * gpu_n_blocks + n_transfer] = gpu_obj;
			n_transfer++;
		}
	}

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->gpu_lock[thread]) ); // G ***
#endif

	// Exit early if a flush is not required.   
	if ( n_transfer == 0 )
		return;

	// Flush the block outside the critical section.
	for ( i = 0; i < n_transfer; i++ )
	{
		gpu_obj = args->gpu_log[thread * gpu_n_blocks + i];
		FLASH_Queue_read_gpu( gpu_obj.obj, gpu_obj.buffer_gpu );
	}

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->gpu_lock[thread]) ); // G ***
#endif

	// Update the bits for each block that is flushed.
	for ( i = 0; i < n_transfer; i++ )
	{
		// Locate the position of the block on the GPU.
		for ( k = 0; k < gpu_n_blocks; k++ )
			if ( args->gpu_log[thread * gpu_n_blocks + i].obj.base == 
					args->gpu[thread * gpu_n_blocks + k].obj.base )
				break;

		if ( k < gpu_n_blocks )
		{
			// The block is now clean.
			args->gpu[thread * gpu_n_blocks + k].clean   = TRUE;
			args->gpu[thread * gpu_n_blocks + k].request = FALSE;
		}
	}

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->gpu_lock[thread]) ); // G ***
#endif

	return;
}

#endif


#ifdef FLA_ENABLE_HIP

void FLASH_Queue_create_hip( int thread, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_create_hip

	  ----------------------------------------------------------------------------*/
{
	//debug
	hipSetDevice( thread );
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int i;
	dim_t hip_n_blocks     = FLASH_Queue_get_hip_num_blocks();
	dim_t block_size       = args->block_size;
	FLA_Datatype datatype  = args->datatype;

	// Exit if not using HIP.
	if ( !FLASH_Queue_get_enabled_hip() )
		return;

	// Bind thread to a HIP device
	FLASH_Queue_bind_hip( thread );

	// Allocate the static block cache on device if managed memory is not used
	if ( ! FLASH_Queue_get_malloc_managed_enabled_hip() )
	{
		// Allocate the memory on the HIP device for all the blocks a priori.
		for ( i = 0; i < hip_n_blocks; i++ )
			FLASH_Queue_alloc_hip( block_size, datatype, &(args->hip[thread * hip_n_blocks + i].buffer_hip) );
	}

	return;
}


void FLASH_Queue_destroy_hip( int thread, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_destroy_hip

	  ----------------------------------------------------------------------------*/
{
<<<<<<< HEAD
	//debug
	//printf("Thread %d entering FLASH_Queue_destroy_hip\n", thread);

	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int i;
	dim_t hip_n_blocks = FLASH_Queue_get_hip_num_blocks();
	FLA_Obj_hip hip_obj;

	// Exit if not using HIP.
	if ( !FLASH_Queue_get_enabled_hip() )
		return;

	// Exit if managed memory is used
	if ( FLASH_Queue_get_malloc_managed_enabled_hip( ) )
		return;

	// Examine every block left on the HIP device.
	for ( i = 0; i < hip_n_blocks; i++ )
	{
		hip_obj = args->hip[thread * hip_n_blocks + i];

#ifdef FLA_ENABLE_D2D

		// Flush the blocks that are dirty.

		if ( hip_obj.obj.base != NULL && bcode_check(hip_obj.obj.base->thread, thread))
		{
			FLASH_Queue_read_hip( thread, hip_obj.obj, hip_obj.buffer_hip );

		}



#else

		if ( hip_obj.obj.base != NULL && !hip_obj.clean )
		{
			FLASH_Queue_read_async_hip( thread, hip_obj.obj, hip_obj.buffer_hip );

		}
#endif


#ifdef FLA_ENABLE_MULTITHREADING
		FLA_Lock_acquire( &(args->hip_lock[thread]) ); // G ***
#endif

#ifdef FLA_ENABLE_D2D
		if ( hip_obj.obj.base != NULL)
		{
			hip_obj.obj.base->thread = bcode_clear(hip_obj.obj.base->thread, thread);
		}
#endif

#ifdef FLA_ENABLE_MULTITHREADING
		FLA_Lock_release( &(args->hip_lock[thread]) ); // G ***
#endif




		// Free the memory on the HIP for all the blocks.
		FLASH_Queue_free_hip( hip_obj.buffer_hip );
	}

	return;
=======
   FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
   int i;
   dim_t hip_n_blocks = FLASH_Queue_get_hip_num_blocks();
   FLA_Obj_hip hip_obj;

   // Exit if not using HIP.
   if ( !FLASH_Queue_get_enabled_hip() )
      return;

   // Exit if managed memory is used
   if ( FLASH_Queue_get_malloc_managed_enabled_hip( ) )
      return;

   // Examine every block left on the HIP device.
   for ( i = 0; i < hip_n_blocks; i++ )
   {
      hip_obj = args->hip[thread * hip_n_blocks + i];

      // Flush the blocks that are dirty.
      if ( hip_obj.obj.base != NULL && !hip_obj.clean )
         FLASH_Queue_read_async_hip( thread, hip_obj.obj, hip_obj.buffer_hip );
      // Free the memory on the HIP for all the blocks.
      FLASH_Queue_free_hip( hip_obj.buffer_hip );
   }

   return;
>>>>>>> bef93ebff3f450a947ce8ca22490dc5eeb952036
}

FLA_Bool FLASH_Queue_exec_hip( FLASH_Task *t, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_exec_hip

	  ----------------------------------------------------------------------------*/
{


	//debug
	//printf("Thread %d entering FLASH_Queue_exec_hip\n", t->thread);
	void** input_arg;
	void** output_arg;

	if ( t == NULL )
		return TRUE;

	// If not using a HIP device, then execute on CPU.
	if ( !FLASH_Queue_get_enabled_hip() )
	{
		//debug
		//printf("warning entering wrong places\n");
		FLASH_Queue_exec_task( t );

		return TRUE;
	}

	// Check if all the operands are ready and up to date.
	if ( !FLASH_Queue_check_hip( t, arg ) )
	{
		//debug
		//printf("Warning entering wrong places!123\n");
		FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
		int queue = t->queue;
		t->hit = FALSE;

#ifdef FLA_ENABLE_MULTITHREADING
		FLA_Lock_acquire( &(args->run_lock[queue]) ); // R ***
#endif
		// Reenqueue the task if the blocks are not all flushed.
		FLASH_Queue_wait_enqueue( t, arg );

#ifdef FLA_ENABLE_MULTITHREADING
		FLA_Lock_release( &(args->run_lock[queue]) ); // R ***
#endif

		return FALSE;
	}

	// If HIP is enabled, but the task is not supported for HIP execution.
	if ( !t->enabled_hip )
	{
		//debug
		//printf("Warning Task not enable in HIP\n");
		int i, j, k;
		int thread        = t->thread;
		int n_input_args  = t->n_input_args;
		int n_output_args = t->n_output_args;
		int n_threads     = FLASH_Queue_get_num_threads();
		FLA_Bool duplicate;
		FLA_Obj  obj;

		//printf("DEBUG: Task not HIP enabled! Name: %s\n", t->name);

		// Check the blocks on each HIP device.
		for ( k = 0; k < n_threads; k++ )
		{
			// Check the input and output arguments on the HIP devices.
			for ( i = 0; i < n_input_args + n_output_args; i++ )
			{
				// Check for duplicate blocks.
				duplicate = FALSE;

				// Find the correct input or output argument.
				if ( i < n_input_args )
				{
					obj = t->input_arg[i];

					for ( j = 0; j < n_output_args && !duplicate; j++ )
					{
						if ( obj.base == t->output_arg[j].base )
							duplicate = TRUE;
					}

					for ( j = 0; j < i && !duplicate; j++ )
					{
						if ( obj.base == t->input_arg[j].base )
							duplicate = TRUE;
					}
				}
				else
				{
					obj = t->output_arg[i - n_input_args];

					for ( j = 0; j < i - n_input_args && !duplicate; j++ )
					{
						if ( obj.base == t->output_arg[j].base )
							duplicate = TRUE;
					}
				}

				// If the block has not been processed before.
				if ( !duplicate )
				{
					// Macroblock is used.
					if ( FLA_Obj_elemtype( obj ) == FLA_MATRIX )
					{
						dim_t    jj, kk;
						dim_t    m    = FLA_Obj_length( obj );
						dim_t    n    = FLA_Obj_width( obj );
						dim_t    cs   = FLA_Obj_col_stride( obj );
						FLA_Obj* buf  = FLASH_OBJ_PTR_AT( obj );

						// Clear each block in macroblock.
						for ( jj = 0; jj < n; jj++ )
						{
							for ( kk = 0; kk < m; kk++ )
							{
								obj = *( buf + jj * cs + kk );

								// Flush the block to main memory if it is on the HIP device.
								if ( k == thread )
									FLASH_Queue_flush_block_hip( obj, k, arg );

								// Invalidate output block on all HIP devices.
								if ( i >= n_input_args )
									FLASH_Queue_invalidate_block_hip( obj, k, arg );
							}
						}
					}
					else
					{
						// Flush the block to main memory if it is on the HIP device.
						if ( k == thread )
							FLASH_Queue_flush_block_hip( obj, k, arg );

						// Invalidate output block on all HIP devices.
						if ( i >= n_input_args )
							FLASH_Queue_invalidate_block_hip( obj, k, arg );
					}
				}
			}
		}

		// Execute the task on CPU instead of HIP device.
		FLASH_Queue_exec_task( t );

		return TRUE;
	}

	int thread = t->thread;
	dim_t hip_n_blocks = FLASH_Queue_get_hip_num_blocks();
	// Gather the pointers for the data on the HIP device.
	input_arg = ( void** ) FLA_malloc( t->n_input_args * sizeof( void* ) );
	output_arg = ( void** ) FLA_malloc( t->n_output_args * sizeof( void* ) );

	//printf("before update_hip t-> thread %d\n", t->thread);
	// Bring all the blocks to the HIP device.
	FLASH_Queue_update_hip( t, input_arg, output_arg, arg );
	//printf("Thread %d after UPDATE_HIP t->input thread: %d  t->input thread: %d\n", t->thread, t->input_arg[0].thread, t->output_arg[0].thread);
	//debug
	//printf("Thread %d entering FLASH_Queue_exec_task_hip\n", t->thread);
	//printf("BEFORE Thread %d exec_task_hip input_arg %p output_arg %p\n", t->thread);

	//FLA_Obj in0, in1, out0;
/*	for(int i = 0; i < t->n_input_args; i++){
		printf("BEFORE exec_task_hip input args %d has ptr %p\n", i, *input_arg);
	}
	for(int i = 0; i < t->n_output_args; i++){
		printf("BEFORE exec_task_hip output args %d has ptr %p\n", i, *output_arg);
	}
	char* ppo = "Gemm ";
	char* scl = "Scal ";
*/
/*
	//printf("##### CHECK STAGE RESULRS %d #####\n", strcmp(ppo, t->name));
	if(strcmp(ppo, t->name) == 0 )
	{
		//printf("##### CHECK STAGE RESULRS #####\n");

		//FLASH_Obj_create_flat_copy_of_hier( t->input_arg[0], &in0 );
		//FLASH_Obj_create_flat_copy_of_hier( t->input_arg[1], &in1 );
		//FLASH_Obj_create_flat_copy_of_hier( t->output_arg[0], &out0 );

		in0 = t->input_arg[0];
		in1 = t->input_arg[1];
		out0 = t->output_arg[0];

		dim_t m_A      = FLA_Obj_length( in0 );
		dim_t n_A      = FLA_Obj_width( in0 );
		dim_t rs_A     = FLA_Obj_row_stride( in0 );
		dim_t cs_A     = FLA_Obj_col_stride( in0 );


		dim_t m_B      = FLA_Obj_length( in1 );
		dim_t n_B      = FLA_Obj_width( in1 );
		dim_t rs_B     = FLA_Obj_row_stride( in1 );
		dim_t cs_B     = FLA_Obj_col_stride( in1 );


		dim_t m_C      = FLA_Obj_length( out0 );
		dim_t n_C      = FLA_Obj_width( out0 );
		dim_t rs_C     = FLA_Obj_row_stride( out0 );
		dim_t cs_C     = FLA_Obj_col_stride( out0 );


		double *buff_a = ( double * ) FLA_DOUBLE_PTR( in0 );
		double *buff_b = ( double * ) FLA_DOUBLE_PTR( in1 );
		double *buff_c = ( double * ) FLA_DOUBLE_PTR( out0 );

		for( int j = 0; j < n_A; j++ )
		{
			for( int i = 0; i < m_A; i++ )
			{

				//printf("#### task: %s %d id:%d in0: %f ####\n", t->name, t->order, j*cs_A + i*rs_A, buff_a[ j*cs_A + i*rs_A ]);	

			 }
		}


		for( int j = 0; j < n_B; j++ )
		{
			for( int i = 0; i < m_B; i++ )
			{

				//printf("#### task: %s %d id:%d in1: %f ####\n", t->name, t->order, j*cs_B + i*rs_B, buff_b[ j*cs_B + i*rs_B ]);

			}
		}


		for( int j = 0; j < n_C; j++ )
		{
			for( int i = 0; i < m_C; i++ )
			{

				//printf("#### task: %s %d id:%d out0: %f ####\n", t->name, t->order, j*cs_C + i*rs_C, buff_c[ j*cs_C + i*rs_C ]);

			}
		}



	}


 #ifdef FLA_ENABLE_D2D
        if(strcmp(ppo, t->name) == 0 || strcmp(scl, t->name) == 0 )
        {
                printf("##### CHECK STAGE RESULRS #####\n");

                //FLASH_Obj_create_flat_copy_of_hier( t->input_arg[0], &in0 );
                //FLASH_Obj_create_flat_copy_of_hier( t->input_arg[1], &in1 );
                //FLASH_Obj_create_flat_copy_of_hier( t->output_arg[0], &out0 );

                in0 = t->input_arg[0];
                in1 = t->input_arg[1];
                out0 = *((FLA_Obj*)output_arg);

                dim_t m_A      = FLA_Obj_length( in0 );
                dim_t n_A      = FLA_Obj_width( in0 );
                dim_t rs_A     = FLA_Obj_row_stride( in0 );
                dim_t cs_A     = FLA_Obj_col_stride( in0 );


                dim_t m_B      = FLA_Obj_length( in1 );
                dim_t n_B      = FLA_Obj_width( in1 );
                dim_t rs_B     = FLA_Obj_row_stride( in1 );
                dim_t cs_B     = FLA_Obj_col_stride( in1 );


                dim_t m_C      = FLA_Obj_length( out0 );
                dim_t n_C      = FLA_Obj_width( out0 );
                dim_t rs_C     = FLA_Obj_row_stride( out0 );
                dim_t cs_C     = FLA_Obj_col_stride( out0 );


                double *buff_a = ( double * ) FLA_DOUBLE_PTR( in0 );
                double *buff_b = ( double * ) FLA_DOUBLE_PTR( in1 );
                double *buff_c = ( double * ) FLA_DOUBLE_PTR( out0 );

                for( int j = 0; j < n_A; j++ )
                {
                        for( int i = 0; i < m_A; i++ )
                        {

                                printf("#### updated task: %s %d id:%d in0: %f ####\n", t->name, t->order,  j*cs_A + i*rs_A, buff_a[ j*cs_A + i*rs_A ]);

                        }
                }


                for( int j = 0; j < n_B; j++ )
                {
                        for( int i = 0; i < m_B; i++ )
                        {

                                printf("#### updated task: %s %d id:%d in1: %f ####\n",  t->name, t->order, j*cs_B + i*rs_B, buff_b[ j*cs_B + i*rs_B ]);

                        }
                }


                for( int j = 0; j < n_C; j++ )
                {
                        for( int i = 0; i < m_C; i++ )
                        {

                                printf("#### updated task: %s %d id:%d out0: %f ####\n", t->name, t->order, j*cs_C + i*rs_C, buff_c[ j*cs_C + i*rs_C ]);

                        }
                }
	*/
	//hipDeviceSynchronize();



/*
#ifdef FLA_ENABLE_D2D
         FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
         FLA_Obj_hip  hip_objs[12];
         // Examine every block left on the HIP device.
         for ( int i = 0; i < 12; i++ )
         {
                 hip_objs[i] = args->hip[thread * hip_n_blocks + i];


                 // Flush the blocks that are dirty.

                 if ( hip_objs[i].obj.base != NULL )
                 {
                        FLASH_Queue_read_hip( thread, hip_objs[i].obj, hip_objs[i].buffer_hip );

                 }
        }
        for( int ed = 0; ed < 12; ed ++)
        {
                if ( hip_objs[ed].obj.base == NULL)
                {
                        break;
                }
                in0 = hip_objs[ed].obj;
                double *buff_a = ( double * ) FLA_DOUBLE_PTR( in0 );
                dim_t m_A      = FLA_Obj_length( in0 );
                dim_t n_A      = FLA_Obj_width( in0 );
                dim_t rs_A     = FLA_Obj_row_stride( in0 );
                dim_t cs_A     = FLA_Obj_col_stride( in0 );

                for( int j = 0; j < n_A; j++ )
                {
                        for( int i = 0; i < m_A; i++ )
                        {

                                printf("#### updated task: %s %d thread%dLRU_id:%d  pos%d in0: %f ####\n", t->name, t->order, thread,ed ,j*cs_A + i*rs_A, buff_a[ j*cs_A + i*rs_A ]);

                        }
                }




        }
	
	for(int i = 0; i < t->n_input_args; i ++){

		double * debug_input_ptr = (double *)input_arg[i];
		printf("#### INPUT updated task: %s %d thread%d input_arg %d has value %f ####\n", t->name, t->order, thread, i , debug_input_ptr[0]);

	
	}
	for(int o = 0; o < t->n_output_args; o++){
		double * debug_output_ptr = (double *)output_arg[o];
		printf("#### OUTPUT updated task: %s %d thread%d output_arg %d has value %f ####\n", t->name, t->order, thread, o , debug_output_ptr[0]);

	}


#endif
*/


	
	// Execute the task on the HIP device.
	FLASH_Queue_exec_task_hip( t, input_arg, output_arg );
	//hipDeviceSynchronize();
/*
 #ifdef FLA_ENABLE_D2D
        if(strcmp(ppo, t->name) == 0 || strcmp(scl, t->name) == 0)
        {
                printf("##### CHECK STAGE RESULRS #####\n");
		FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	
                //FLASH_Obj_create_flat_copy_of_hier( t->input_arg[0], &in0 );
                //FLASH_Obj_create_flat_copy_of_hier( t->input_arg[1], &in1 );
                //FLASH_Obj_create_flat_copy_of_hier( t->output_arg[0], &out0 );

                in0 = t->input_arg[0];
                in1 = t->input_arg[1];
                out0 = *((FLA_Obj*)output_arg);

                dim_t m_A      = FLA_Obj_length( in0 );
                dim_t n_A      = FLA_Obj_width( in0 );
                dim_t rs_A     = FLA_Obj_row_stride( in0 );
                dim_t cs_A     = FLA_Obj_col_stride( in0 );


                dim_t m_B      = FLA_Obj_length( in1 );
                dim_t n_B      = FLA_Obj_width( in1 );
                dim_t rs_B     = FLA_Obj_row_stride( in1 );
                dim_t cs_B     = FLA_Obj_col_stride( in1 );


                dim_t m_C      = FLA_Obj_length( out0 );
                dim_t n_C      = FLA_Obj_width( out0 );
                dim_t rs_C     = FLA_Obj_row_stride( out0 );
                dim_t cs_C     = FLA_Obj_col_stride( out0 );


                double *buff_a = ( double * ) FLA_DOUBLE_PTR( in0 );
                double *buff_b = ( double * ) FLA_DOUBLE_PTR( in1 );
                double *buff_c = ( double * ) FLA_DOUBLE_PTR( out0 );

                for( int j = 0; j < n_A; j++ )
                {
                        for( int i = 0; i < m_A; i++ )
                        {

                                printf("#### updated task: %s %d id:%d in0: %f ####\n", t->name, t->order,  j*cs_A + i*rs_A, buff_a[ j*cs_A + i*rs_A ]);

                        }
                }


                for( int j = 0; j < n_B; j++ )
                {
                        for( int i = 0; i < m_B; i++ )
                        {

                                printf("#### updated task: %s %d id:%d in1: %f ####\n",  t->name, t->order, j*cs_B + i*rs_B, buff_b[ j*cs_B + i*rs_B ]);

                        }
                }


                for( int j = 0; j < n_C; j++ )
                {
                        for( int i = 0; i < m_C; i++ )
                        {

                                printf("#### updated task: %s %d id:%d out0: %f ####\n", t->name, t->order, j*cs_C + i*rs_C, buff_c[ j*cs_C + i*rs_C ]);

                        }
                }
*/
/*
#ifdef FLA_ENABLE_D2D
         // Examine every block left on the HIP device.
         for ( int i = 0; i < 12; i++ )
         {
                 hip_objs[i] = args->hip[thread * hip_n_blocks + i];


                 // Flush the blocks that are dirty.

                 if ( hip_objs[i].obj.base != NULL )
                 {
                        FLASH_Queue_read_hip( thread, hip_objs[i].obj, hip_objs[i].buffer_hip );

                 }
	}
	for( int ed = 0; ed < 12; ed ++)
	{
		if ( hip_objs[ed].obj.base == NULL)
                {
			continue;
		}
		in0 = hip_objs[ed].obj;
		double *buff_a = ( double * ) FLA_DOUBLE_PTR( in0 );
                dim_t m_A      = FLA_Obj_length( in0 );
                dim_t n_A      = FLA_Obj_width( in0 );
                dim_t rs_A     = FLA_Obj_row_stride( in0 );
                dim_t cs_A     = FLA_Obj_col_stride( in0 );

                for( int j = 0; j < n_A; j++ )
                {
                        for( int i = 0; i < m_A; i++ )
                        {

                                printf("#### calculated task: %s %d thread%dLRU_id:%d  pos%d in0: %f ####\n", t->name, t->order, thread,ed ,j*cs_A + i*rs_A, buff_a[ j*cs_A + i*rs_A ]);

                        }
                }
		



        }
        for(int i = 0; i < t->n_input_args; i ++){

                double * debug_input_ptr = (double *)input_arg[i];
                printf("#### INPUT calculated task: %s %d thread%d input_arg %d has value %f ####\n", t->name, t->order, thread, i , debug_input_ptr[0]);


        }
        for(int o = 0; o < t->n_output_args; o++){
                double * debug_output_ptr = (double *)output_arg[o];
                printf("#### OUTPUT calculated task: %s %d thread%d output_arg %d has value %f ####\n", t->name, t->order, thread, o , debug_output_ptr[0]);

        }


#endif
*/





	//printf("Thread %d after EXEC_TASK_HIP t->input thread: %d  t->input thread: %d\n", t->thread, t->input_arg[0].thread, t->output_arg[0].thread);
	//printf("Task %s before mark_hip t-> thread %c\n", t->name, t->thread);
	// Mark all the output blocks as dirty.
	FLASH_Queue_mark_hip( t, arg );
	//printf("Task %s after mark_hip t-> thread %c\n", t->name, t->thread);
	//printf("Thread %d after MARK_HIP t->input thread: %d  t->input thread: %d\n", t->thread, t->input_arg[0].thread, t->output_arg[0].thread);


	// Free memory.
	FLA_free( input_arg );
	FLA_free( output_arg );

	return TRUE;
}

FLA_Bool FLASH_Queue_check_hip( FLASH_Task *t, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_check_hip

	  ----------------------------------------------------------------------------*/
{
	int i, j, k;
	int thread        = t->thread;

	//debug 
	//printf("Thread %d entering FLASH_Queue_check_hip\n", thread);


	int n_input_args  = t->n_input_args;
	int n_output_args = t->n_output_args;
	int n_threads     = FLASH_Queue_get_num_threads();
	FLA_Bool r_val    = TRUE;
	FLA_Bool t_val;
	FLA_Bool duplicate;
	FLA_Obj  obj;

	// Check the input and output arguments on the HIP devices.
	for ( i = 0; i < n_input_args + n_output_args; i++ )
	{
		// Check for duplicate blocks.
		duplicate = FALSE;

		// Find the correct input or output argument.
		if ( i < n_input_args )
		{
			obj = t->input_arg[i];

			for ( j = 0; j < n_output_args && !duplicate; j++ )
			{
				if ( obj.base == t->output_arg[j].base )
					duplicate = TRUE;
			}

			for ( j = 0; j < i && !duplicate; j++ )
			{
				if ( obj.base == t->input_arg[j].base )
					duplicate = TRUE;
			}
		}
		else
		{
			obj = t->output_arg[i - n_input_args];

			for ( j = 0; j < i - n_input_args && !duplicate; j++ )
			{
				if ( obj.base == t->output_arg[j].base )
					duplicate = TRUE;
			}
		}

#ifdef FLA_ENABLE_D2D

		//do I need to do -2 is on host -1 is uninitialize here? or can we assume in D2D the data is always ready when we set the block to dirty

		// If the block has not been processed before.
		if ( !duplicate )
		{
			// Macroblock is used.
			if ( FLA_Obj_elemtype( obj ) == FLA_MATRIX )
			{
				dim_t    jj, kk;
				dim_t    m    = FLA_Obj_length( obj );
				dim_t    n    = FLA_Obj_width( obj );
				dim_t    cs   = FLA_Obj_col_stride( obj );
				FLA_Obj* buf  = FLASH_OBJ_PTR_AT( obj );

				// Clear each block in macroblock.
				for ( jj = 0; jj < n; jj++ )
				{
					for ( kk = 0; kk < m; kk++ )
					{
						obj = *( buf + jj * cs + kk );

						t_val = TRUE;

						// Check to see if the block is dirty on another HIP device.
						if(obj.base->occupied  != -1)
						{
							//printf("Warning something is not ready in check hip\n");
							t_val = FALSE;	
						}
						r_val = r_val && t_val;
					}
				}
			}
			else
			{

				t_val = TRUE;

				// Check to see if the block is dirty on another HIP device.
				if(obj.base->occupied != -1)
					t_val = FALSE;



				r_val = r_val && t_val;


			}
		}


#else

		// If the block has not been processed before.
		if ( !duplicate )
		{
			// Macroblock is used.
			if ( FLA_Obj_elemtype( obj ) == FLA_MATRIX )
			{
				dim_t    jj, kk;
				dim_t    m    = FLA_Obj_length( obj );
				dim_t    n    = FLA_Obj_width( obj );
				dim_t    cs   = FLA_Obj_col_stride( obj );
				FLA_Obj* buf  = FLASH_OBJ_PTR_AT( obj );

				// Clear each block in macroblock.
				for ( jj = 0; jj < n; jj++ )
				{
					for ( kk = 0; kk < m; kk++ )
					{
						obj = *( buf + jj * cs + kk );

						t_val = TRUE;

						// Check to see if the block is dirty on another HIP device.
						for ( k = 0; k < n_threads && t_val; k++ )
							if ( k != thread )
								t_val = t_val && FLASH_Queue_check_block_hip( obj, k, arg );
						r_val = r_val && t_val;
					}
				}
			}
			else
			{

				t_val = TRUE;

				// Check to see if the block is dirty on another HIP device.
				for ( k = 0; k < n_threads && t_val; k++ )
					if ( k != thread )
						t_val = t_val && FLASH_Queue_check_block_hip( obj, k, arg );
				r_val = r_val && t_val;


			}
		}
#endif 
	}


	return r_val;
}


FLA_Bool FLASH_Queue_check_block_hip( FLA_Obj obj, int thread, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_check_block_hip

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int k;
	dim_t hip_n_blocks = FLASH_Queue_get_hip_num_blocks();
	FLA_Bool r_val = TRUE;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->hip_lock[thread]) ); // G ***
#endif

	// Locate the position of the block on the HIP device.
	for ( k = 0; k < hip_n_blocks; k++ )
		if ( obj.base == args->hip[thread * hip_n_blocks + k].obj.base )
			break;

	if ( k < hip_n_blocks )
	{
		// Request this block if it is dirty.
		if ( !args->hip[thread * hip_n_blocks + k].clean )
		{
			args->hip[thread * hip_n_blocks + k].request = TRUE;

			r_val = FALSE;
		}
	}

	// Check the victim block.
	if ( obj.base == args->victim[thread].obj.base )
		r_val = FALSE;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->hip_lock[thread]) ); // G ***
#endif

	return r_val;
}


void FLASH_Queue_update_hip( FLASH_Task *t,
		void **input_arg,
		void **output_arg,
		void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_update_hip

	  ----------------------------------------------------------------------------*/
{
	int i, j, k;
	int thread    = t->thread;

	//debug
/*
	printf("------------------------------------------------------------\n");
	printf("Thread %d entering FLASH_Queue_update_hip\n", thread);
	printf("Task %s %d\n", t->name, t->order);
	for(int i = 0; i < t->n_input_args; i++){
		printf("update_hip input args %d has thread %c\n", i, t->input_arg[i].base->thread);
	}
	for(int i = 0; i < t->n_output_args; i++){
		printf("update_hip output args %d has thread %c\n", i, t->output_arg[i].base->thread);
	}

	printf("------------------------------------------------------------\n");
*/
	int n_threads = FLASH_Queue_get_num_threads();
	FLA_Bool duplicate;

	// None of the arguments can be macroblocks yet.
	// Complicating factor is copying macroblock to contiguous memory on HIP device.

	// Bring the input arguments to the HIP device.
	for ( i = t->n_input_args - 1; i >= 0; i-- )
	{
		// Check for duplicate blocks.
		duplicate = FALSE;

		for ( j = 0; j < t->n_output_args && !duplicate; j++ )
		{
			if ( t->input_arg[i].base == t->output_arg[j].base )
				duplicate = TRUE;
		}

		for ( j = 0; j < i && !duplicate; j++ )
		{
			if ( t->input_arg[i].base == t->input_arg[j].base )
				duplicate = TRUE;
		}

		// If the input block has not been processed before.
		if ( !duplicate )
		{

			//printf("Thread %d with task input block thread = %c\n", thread , t->input_arg[i].base->thread);

#ifdef FLA_ENABLE_D2D
			FLASH_Queue_update_block_d2d_hip( t->input_arg[i], input_arg + i, thread, arg, true );
#else
			FLASH_Queue_update_block_hip( t->input_arg[i], input_arg + i, thread, arg );
#endif


			//printf("THREAD %d\n", thread);

		}
		else
		{
			input_arg[i] = NULL;
		}
	}

	// Bring the output arguments to the HIP device.
	for ( i = t->n_output_args - 1; i >= 0; i-- )
	{
		// Check for duplicate blocks.
		duplicate = FALSE;

		for ( j = 0; j < i && !duplicate; j++ )
		{
			if ( t->output_arg[i].base == t->output_arg[j].base )
				duplicate = TRUE;
		}

		// If the output block has not been processed before.
		if ( !duplicate )
		{


			// treat d2d differently

#ifdef FLA_ENABLE_D2D								
			//printf("thread %d before FLASH_Queue_update_block_hip_outputarg %c\n", thread, t->output_arg[i].base->thread);
			FLASH_Queue_update_block_d2d_hip( t->output_arg[i], output_arg + i, thread, arg, false );
			//if(t->output_arg[i].thread != 0 && t->output_arg[i].thread != thread + 1)
			//FLASH_Queue_invalidate_block_hip( t->output_arg[i], t->output_arg[i].base->thread - 1, arg );


			//printf("thread %d after FLASH_Queue_update_block_hip_outputarg %c\n", thread, t->output_arg[i].base->thread);

#else
			FLASH_Queue_update_block_hip( t->output_arg[i], output_arg + i, thread, arg );
			for ( k = 0; k < n_threads; k++ )
				if ( k != thread )
					FLASH_Queue_invalidate_block_hip( t->output_arg[i], k, arg );

#endif




		}


		else
		{
			output_arg[i] = NULL;
		}
	}
	//debug

	/*#ifdef FLA_ENABLE_D2D
	  for ( i = t->n_input_args - 1; i >= 0; i-- )
	  {


	  for ( k = 0; k < n_threads; k++ )
	  {
	  if ( k != thread )
	  FLASH_Queue_invalidate_block_hip( t->input_arg[i], k, arg );
	  }




	  }
#endif*/



	// Check to see if there are any duplicates.
	for ( i = t->n_input_args - 1; i >= 0; i-- )
	{
		for ( j = 0; j < t->n_output_args && input_arg[i] == NULL; j++ )
		{
			if ( t->input_arg[i].base == t->output_arg[j].base )
				input_arg[i] = output_arg[j];
		}

		for ( j = 0; j < i && input_arg[i] == NULL; j++ )
		{
			if ( t->input_arg[i].base == t->input_arg[j].base )
				input_arg[i] = input_arg[j];
		}
	}

	// Check to see if there are any duplicates.
	for ( i = t->n_output_args - 1; i >= 0; i-- )
	{
		for ( j = 0; j < i && output_arg[i] == NULL; j++ )
		{
			if ( t->output_arg[i].base == t->output_arg[j].base )
				output_arg[i] = output_arg[j];
		}
	}

	return;
}


void FLASH_Queue_update_block_hip( FLA_Obj obj,
		void **buffer_hip,
		int thread,
		void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_update_block_hip

	  ----------------------------------------------------------------------------*/
{
	//printf("warning entering orig update blck hip");
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int j, k;
	dim_t hip_n_blocks = FLASH_Queue_get_hip_num_blocks();
	FLA_Bool transfer = FALSE;
	FLA_Bool evict = FALSE;
	FLA_Obj_hip evict_obj;
	FLA_Obj_hip hip_obj;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->hip_lock[thread]) ); // G ***
#endif

	// Locate the position of the block on the HIP device.
	for ( k = 0; k < hip_n_blocks - 1; k++ )
		if ( obj.base == args->hip[thread * hip_n_blocks + k].obj.base )
			break;

	// Save the pointer to the data on the HIP device.
	buffer_hip[0] = args->hip[thread * hip_n_blocks + k].buffer_hip;

	// Save the victim block.
	evict_obj = args->hip[thread * hip_n_blocks + k];

	// The block is not already in the HIP device.
	if ( obj.base != args->hip[thread * hip_n_blocks + k].obj.base )
	{
		// Save for data transfer outside of critical section.
		transfer = TRUE;

		// Save for eviction outside of critical section.
		if ( evict_obj.obj.base != NULL && !evict_obj.clean )
		{
			evict = TRUE;
			args->victim[thread] = evict_obj;
		}

		// Save the block in the data structure.
		args->hip[thread * hip_n_blocks + k].obj = obj;

		// Make sure the new block is clean.
		args->hip[thread * hip_n_blocks + k].clean   = TRUE;
		args->hip[thread * hip_n_blocks + k].request = FALSE;
	}

	// Use the block on the HIP device that is a hit or LRU.
	hip_obj = args->hip[thread * hip_n_blocks + k];

	// Shift all the previous tasks for LRU replacement.
	for ( j = k; j > 0; j-- )
		args->hip[thread * hip_n_blocks + j] = args->hip[thread * hip_n_blocks + j - 1];

	// Place the block on the cache as the most recently used.
	args->hip[thread * hip_n_blocks] = hip_obj;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->hip_lock[thread]) ); // G ***
#endif

	// Evict and flush the LRU dirty block.
	if ( evict )
	{
		FLASH_Queue_read_hip( thread, evict_obj.obj, evict_obj.buffer_hip );
#ifdef FLA_ENABLE_MULTITHREADING
		FLA_Lock_acquire( &(args->hip_lock[thread]) ); // G ***
#endif

		args->victim[thread].obj.base = NULL;

#ifdef FLA_ENABLE_MULTITHREADING
		FLA_Lock_release( &(args->hip_lock[thread]) ); // G ***
#endif
	}

	// Move the block to the HIP device.
	if ( transfer )
		FLASH_Queue_write_hip( hip_obj.obj, hip_obj.buffer_hip );

	return;
}



void FLASH_Queue_update_block_d2d_hip( FLA_Obj obj,
		void **buffer_hip,
		int thread,
		void *arg,
		bool isInput )
	/*----------------------------------------------------------------------------
	  FLASH_Queue_update_block_d2d_hip
	  ----------------------------------------------------------------------------*/
{
	//printf("THREAD %d entering FLASH_Queue_update_block_d2d_hip with obj.thread = %c\n", thread, obj.base->thread);
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int j, k;
	dim_t hip_n_blocks = FLASH_Queue_get_hip_num_blocks();
	FLA_Bool transfer_hd = FALSE;
	FLA_Bool transfer_dd = FALSE;
	FLA_Bool transfer_self = FALSE;
	FLA_Bool evict = FALSE;
	FLA_Obj_hip evict_obj;
	FLA_Obj_hip hip_obj;
	int current_location;
	int position_on_other_thread;
	int lock_id;
	int n_threads  = FLASH_Queue_get_num_threads();
	void * buffer_on_other_thread;


	//debug need to check if this will work on more than 2 gpu

	if(bcode_check(obj.base->thread, thread))
	{
	
		transfer_self = TRUE;
		lock_id = thread;

	}
	else
	{

		for(int pos = 0; pos < n_threads; pos++){

			if(bcode_check(obj.base->thread, pos))
			{
				transfer_dd = TRUE;
				lock_id = pos;
				break;
			}

		}
		if(!transfer_dd)
		{

			transfer_hd = TRUE;
	                lock_id = thread;

		}

	}



#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->hip_lock[lock_id]) ); // G ***
#endif

	
	if(transfer_self || transfer_dd){

		for ( k = 0; k < hip_n_blocks; k++ ){
			if ( obj.base == args->hip[lock_id * hip_n_blocks + k].obj.base )
				break;
		}


		position_on_other_thread = k;
		if(transfer_dd)
			buffer_on_other_thread = args->hip[(lock_id) * hip_n_blocks + k].buffer_hip;

	}
			
		
	//doing a double chcek here, shouldn't need it if the code is well written
	if(transfer_hd){
		//if not in any hip devices, get it from host and replace the least used data in LRU
		buffer_hip[0] = args->hip[thread * hip_n_blocks + hip_n_blocks - 1].buffer_hip;

		// Save the victim block.
		evict_obj = args->hip[thread * hip_n_blocks + hip_n_blocks - 1];

		// Save for data transfer outside of critical section.
		//transfer_hd = TRUE;

		// Save for eviction outside of critical section.
		if ( evict_obj.obj.base != NULL && bcode_check(evict_obj.obj.base->thread, thread) )
		{
			evict_obj.obj.base->thread = bcode_clear(evict_obj.obj.base->thread, thread);
			if(evict_obj.obj.base->thread == 0)
			{

				evict = TRUE;
                        	args->victim[thread] = evict_obj;	

			}
		}

		// Save the block in the data structure.For this case we have to pushed from the host
		args->hip[thread * hip_n_blocks + hip_n_blocks - 1].obj = obj;
		// mark as dirty

		// Make sure the new block is clean. NOT SURE IF WE NEED IT BUT I WILL LEAVE IT HERE
		args->hip[thread * hip_n_blocks + hip_n_blocks - 1].clean   = TRUE;
		args->hip[thread * hip_n_blocks + hip_n_blocks - 1].request = FALSE;

		// Use the block on the HIP device that is a hit or LRU.
		hip_obj = args->hip[thread * hip_n_blocks + hip_n_blocks - 1];
		current_location =  hip_n_blocks - 1;


	}

	else if(transfer_dd){
		//in this case it is on one hip devices and we have to map the buffer from previous hip device to here
		//still put it to the least used place (at first)

		//if not in any hip devices, get it from host and replace the least used data in LRU
		buffer_hip[0] = args->hip[thread * hip_n_blocks + hip_n_blocks - 1].buffer_hip;

		// Save the victim block.
		evict_obj = args->hip[thread * hip_n_blocks + hip_n_blocks - 1];

		// Save for data transfer outside of critical section.
		//transfer_dd = TRUE;

		// Save for eviction outside of critical section.
                if ( evict_obj.obj.base != NULL && bcode_check(evict_obj.obj.base->thread, thread) )
                {
                        evict_obj.obj.base->thread = bcode_clear(evict_obj.obj.base->thread, thread);
                        if(evict_obj.obj.base->thread == 0)
                        {

                                evict = TRUE;
                                args->victim[thread] = evict_obj;

                        }
                }


		// Save the block in the data structure.For this case we have to pushed from the host
		args->hip[thread * hip_n_blocks + hip_n_blocks - 1].obj = obj;

		// Make sure the new block is clean. NOT SURE IF WE NEED IT BUT I WILL LEAVE IT HERE
		args->hip[thread * hip_n_blocks + hip_n_blocks - 1].clean   = TRUE;
		args->hip[thread * hip_n_blocks + hip_n_blocks - 1].request = FALSE;

		// Use the block on the HIP device that is a hit or LRU.
		hip_obj = args->hip[thread * hip_n_blocks + hip_n_blocks - 1];
		current_location =  hip_n_blocks - 1;
	}
	else{
		//printf("D2D update entering SAME GPU\n");	
		//this is the "land in same gpu" case
		//do nothing here since it is on the same devices
		current_location = position_on_other_thread;
		// Use the block on the HIP device that is a hit or LRU.
		hip_obj = args->hip[thread * hip_n_blocks + position_on_other_thread];
		buffer_hip[0] = args->hip[thread * hip_n_blocks + position_on_other_thread].buffer_hip;

	}

	// Shift all the previous tasks for LRU replacement.
	for ( j = current_location; j > 0; j-- )
		args->hip[thread * hip_n_blocks + j] = args->hip[thread * hip_n_blocks + j - 1];

	// Place the block on the cache as the most recently used.
	args->hip[thread * hip_n_blocks] = hip_obj;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->hip_lock[lock_id]) ); // G ***
#endif

	// Evict and flush the LRU dirty block.
	if ( evict )
	{
		//printf("in D2D_COPY evict\n");
		//when the LRU is full we evict, we never flush
		FLASH_Queue_read_hip( thread, evict_obj.obj, evict_obj.buffer_hip );
		//evict_obj.obj.base->thread = 0;
#ifdef FLA_ENABLE_MULTITHREADING
		FLA_Lock_acquire( &(args->hip_lock[thread]) ); // G ***
#endif

		args->victim[thread].obj.base = NULL;

#ifdef FLA_ENABLE_MULTITHREADING
		FLA_Lock_release( &(args->hip_lock[thread]) ); // G ***
#endif
	}

	// Move the block to the HIP device.
	if ( transfer_hd )
	{
		//printf("D2D update entering transfer_HD\n");
		//if not in any devices we still using host to device transfer
		FLASH_Queue_write_hip( hip_obj.obj, hip_obj.buffer_hip );
	}
	else if(transfer_dd){
		//printf("D2D update entering transfer_DD\n");
		//printf("ENABLE thread %d peer access to  %c\n", thread, obj.base->thread);
		//printf("IN transfer dd obj base is  %p  bufferhip is %p \n", args->hip[(obj.base->thread) * hip_n_blocks + position_on_other_thread].obj.base, args->hip[(obj.base->thread - 1) * hip_n_blocks + position_on_other_thread].buffer_hip);
		hipDeviceEnablePeerAccess(lock_id, 0);
		//FLASH_Queue_d2d_hip( obj.base->thread - 1, args->hip[(obj.base->thread - 1) * hip_n_blocks + position_on_other_thread].buffer_hip , hip_obj.buffer_hip, FLA_Obj_elem_size( obj ) * FLA_Obj_col_stride( obj ) * FLA_Obj_width( obj ) );
		
		FLASH_Queue_d2d_hip( lock_id, buffer_on_other_thread, hip_obj.buffer_hip, FLA_Obj_elem_size( obj ) * FLA_Obj_col_stride( obj ) * FLA_Obj_width( obj ) );

		hipDeviceDisablePeerAccess(lock_id);
	}

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->hip_lock[lock_id]) ); // G ***
#endif

	// mark as dirty? (currently we do clean in mark)
	if(isInput)
	{

		args->hip[thread * hip_n_blocks].obj.base->thread = bcode_set(args->hip[thread * hip_n_blocks].obj.base->thread, thread);
		args->hip[thread * hip_n_blocks].obj.base->occupied = -1;

	}
	else
	{
		args->hip[thread * hip_n_blocks].obj.base->occupied = thread;

	}

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->hip_lock[lock_id]) ); // G ***
#endif

	return;
}








void FLASH_Queue_mark_hip( FLASH_Task *t, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_mark_hip

	  ----------------------------------------------------------------------------*/
{

	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int i, j, k;
	int thread = t->thread;
	int n_threads = FLASH_Queue_get_num_threads();

	//debug
	//printf("Thread %d Task %s entering FLASH_Queue_mark_hip\n", thread, t->name);


	dim_t hip_n_blocks = FLASH_Queue_get_hip_num_blocks();
	FLA_Bool duplicate;
	FLA_Obj  obj;
/*#ifdef FLA_ENABLE_D2D
	// Mark all the input blocks on the HIP device as dirty.
	for ( i = t->n_input_args - 1; i >= 0; i-- )
	{
		obj = t->input_arg[i];
#ifdef FLA_ENABLE_MULTITHREADING
		FLA_Lock_acquire( &(args->hip_lock[thread]) ); // G ***
#endif

		// Locate the position of the block on the HIP device.
		for ( k = 0; k < hip_n_blocks; k++ )
			if ( obj.base == args->hip[thread * hip_n_blocks + k].obj.base )
				break;

		if ( k < hip_n_blocks )
		{
			// Change the bits for the new dirty block.
			args->hip[thread * hip_n_blocks + k].clean   = FALSE;
			args->hip[thread * hip_n_blocks + k].request = FALSE;
			args->hip[thread * hip_n_blocks + k].obj.base->thread = thread + 1;
			printf("FLASH_Queue_mark_hip marked input obj.thread to %d\n", args->hip[thread * hip_n_blocks + k].obj.base->thread);
		}

#ifdef FLA_ENABLE_MULTITHREADING
		FLA_Lock_release( &(args->hip_lock[thread]) ); // G ***
#endif
	}

#endif*/







	// Mark all the output blocks on the HIP device as dirty.
	for ( i = t->n_output_args - 1; i >= 0; i-- )
	{
		obj = t->output_arg[i];
		//t->output_arg[i].base->thread = thread + 1;
		// Check for duplicate blocks.
		duplicate = FALSE;

		for ( j = 0; j < i && !duplicate; j++ )
		{
			if ( obj.base == t->output_arg[j].base )
				duplicate = TRUE;
		}

		// If the output block has not been processed before.
		if ( !duplicate )
		{
#ifdef FLA_ENABLE_MULTITHREADING
			FLA_Lock_acquire( &(args->hip_lock[thread]) ); // G ***
#endif

			// Locate the position of the block on the HIP device.
			for ( k = 0; k < hip_n_blocks; k++ )
				if ( obj.base == args->hip[thread * hip_n_blocks + k].obj.base )
					break;

			if ( k < hip_n_blocks )
			{
				// Change the bits for the new dirty block.
				args->hip[thread * hip_n_blocks + k].clean   = FALSE;
				args->hip[thread * hip_n_blocks + k].request = FALSE;
#ifdef FLA_ENABLE_D2D
				
                                args->hip[thread * hip_n_blocks + k].obj.base->occupied = -1;
				for(int pos = 0; pos < n_threads; pos++)
				{
					//FLA_Lock_acquire( &(args->hip_lock[pos]) );

					args->hip[thread * hip_n_blocks + k].obj.base->thread = bcode_clear(args->hip[thread * hip_n_blocks + k].obj.base->thread, pos);	

					//FLA_Lock_release( &(args->hip_lock[pos]) );
				}
				args->hip[thread * hip_n_blocks + k].obj.base->thread = bcode_set(args->hip[thread * hip_n_blocks + k].obj.base->thread, thread);
#endif
				//printf("FLASH_Queue_mark_hip marked obj.thread to %c\n", args->hip[thread * hip_n_blocks + k].obj.base->thread);
			}

#ifdef FLA_ENABLE_MULTITHREADING
			FLA_Lock_release( &(args->hip_lock[thread]) ); // G ***
#endif
		}
	}

	return;
}


void FLASH_Queue_invalidate_block_hip( FLA_Obj obj, int thread, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_invalidate_block_hip

	  ----------------------------------------------------------------------------*/
{
	//printf("THREAD %d entering FLASH_Queue_invalidate_block_hip", thread);

	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int j, k;
	dim_t hip_n_blocks = FLASH_Queue_get_hip_num_blocks();
	FLA_Obj_hip hip_obj;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->hip_lock[thread]) ); // G ***
#endif

	// Locate the position of the block on the HIP device.
	for ( k = 0; k < hip_n_blocks; k++ )
		if ( obj.base == args->hip[thread * hip_n_blocks + k].obj.base )
			break;

	// The block is owned by another HIP device.
	if ( k < hip_n_blocks )
	{



		// Invalidate the block.
		args->hip[thread * hip_n_blocks + k].obj.base = NULL;
		args->hip[thread * hip_n_blocks + k].clean    = TRUE;
		args->hip[thread * hip_n_blocks + k].request  = FALSE;

		// Save the block that will be invalidated.
		hip_obj = args->hip[thread * hip_n_blocks + k];

		// Shift all the blocks for the invalidated block.
		for ( j = k; j < hip_n_blocks - 1; j++ )
			args->hip[thread * hip_n_blocks + j] = args->hip[thread * hip_n_blocks + j + 1];

		// Move to the LRU block.
		args->hip[thread * hip_n_blocks + hip_n_blocks - 1] = hip_obj;
	}

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->hip_lock[thread]) ); // G ***
#endif

	return;
}





void FLASH_Queue_flush_block_hip( FLA_Obj obj, int thread, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_flush_block_hip

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int k;
	dim_t hip_n_blocks = FLASH_Queue_get_hip_num_blocks();
	FLA_Bool transfer = FALSE;
	FLA_Obj_hip hip_obj;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->hip_lock[thread]) ); // G ***
#endif

	// Locate the position of the block on the HIP device.
	for ( k = 0; k < hip_n_blocks; k++ )
		if ( obj.base == args->hip[thread * hip_n_blocks + k].obj.base )
			break;

	// The block is owned by the HIP device.
	if ( k < hip_n_blocks )
	{
		// Save the block that will be flushed.
		hip_obj = args->hip[thread * hip_n_blocks + k];

		// If the block is dirty, then flush it.
		if ( hip_obj.obj.base != NULL && !hip_obj.clean )
			transfer = TRUE;
	}

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->hip_lock[thread]) ); // G ***
#endif

	// Exit early if a flush is not required.
	if ( !transfer )
		return;

	// Flush the block outside the critical section.
	FLASH_Queue_read_hip( thread, hip_obj.obj, hip_obj.buffer_hip );

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->hip_lock[thread]) ); // G ***
#endif

	// Locate the position of the block on the HIP device.
	for ( k = 0; k < hip_n_blocks; k++ )
		if ( obj.base == args->hip[thread * hip_n_blocks + k].obj.base )
			break;

	if ( k < hip_n_blocks )
	{
		// Update the bits for the flushed block.
		args->hip[thread * hip_n_blocks + k].clean   = TRUE;
		args->hip[thread * hip_n_blocks + k].request = FALSE;
	}

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->hip_lock[thread]) ); // G ***
#endif

	return;
}


void FLASH_Queue_flush_hip( int thread, void *arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_flush_hip

	  ----------------------------------------------------------------------------*/
{

	//debug 
	//printf("Thread %d entering FLASH_Queue_flush_hip\n", thread);
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int i, k;
	dim_t hip_n_blocks = FLASH_Queue_get_hip_num_blocks();
	int n_transfer = 0;
	FLA_Obj_hip hip_obj;

	// Exit if not using HIP.
	if ( !FLASH_Queue_get_enabled_hip() )
		return;

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->hip_lock[thread]) ); // G ***
#endif

	for ( k = 0; k < hip_n_blocks; k++ )
	{
		// Save the block that might be flushed.
		hip_obj = args->hip[thread * hip_n_blocks + k];

		// Flush the block if it is dirty and requested.
		if ( hip_obj.obj.base != NULL && !hip_obj.clean && hip_obj.request )
		{
			// Save the block for data transfer outside the critical section.
			args->hip_log[thread * hip_n_blocks + n_transfer] = hip_obj;
			n_transfer++;
		}
	}

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->hip_lock[thread]) ); // G ***
#endif

<<<<<<< HEAD
	// Exit early if a flush is not required.   
	if ( n_transfer == 0 )
		return;

	// Flush the block(s) outside the critical section.
	if ( n_transfer == 1 )
	{
		hip_obj = args->hip_log[thread * hip_n_blocks];
		FLASH_Queue_read_hip( thread, hip_obj.obj, hip_obj.buffer_hip );
	}
	else if ( n_transfer == 2 && !FLASH_Queue_get_malloc_managed_enabled_hip( ) )
	{
		// two sync memcpys are faster typically than two async plus device sync
		hip_obj = args->hip_log[thread * hip_n_blocks];
		FLASH_Queue_read_hip( thread, hip_obj.obj, hip_obj.buffer_hip );
		hip_obj = args->hip_log[thread * hip_n_blocks + 1];
		FLASH_Queue_read_hip( thread, hip_obj.obj, hip_obj.buffer_hip );
	}
	else
	{
		for ( i = 0; i < n_transfer; i++ )
		{
			hip_obj = args->hip_log[thread * hip_n_blocks + i];
			FLASH_Queue_read_async_hip( thread, hip_obj.obj, hip_obj.buffer_hip );
		}
		FLASH_Queue_sync_device_hip( thread );
	}
=======
   // Exit early if a flush is not required.   
   if ( n_transfer == 0 )
      return;

   // Flush the block(s) outside the critical section.
   if ( n_transfer == 1 )
   {
      hip_obj = args->hip_log[thread * hip_n_blocks];
      FLASH_Queue_read_hip( thread, hip_obj.obj, hip_obj.buffer_hip );
   }
   else if ( n_transfer == 2 && !FLASH_Queue_get_malloc_managed_enabled_hip( ) )
   {
      // two sync memcpys are faster typically than two async plus device sync
      hip_obj = args->hip_log[thread * hip_n_blocks];
      FLASH_Queue_read_hip( thread, hip_obj.obj, hip_obj.buffer_hip );
      hip_obj = args->hip_log[thread * hip_n_blocks + 1];
      FLASH_Queue_read_hip( thread, hip_obj.obj, hip_obj.buffer_hip );
   }
   else
   {
      for ( i = 0; i < n_transfer; i++ )
      {
         hip_obj = args->hip_log[thread * hip_n_blocks + i];
         FLASH_Queue_read_async_hip( thread, hip_obj.obj, hip_obj.buffer_hip );
      }
      FLASH_Queue_sync_device_hip( thread );
   }
>>>>>>> bef93ebff3f450a947ce8ca22490dc5eeb952036

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_acquire( &(args->hip_lock[thread]) ); // G ***
#endif

	// Update the bits for each block that is flushed.
	for ( i = 0; i < n_transfer; i++ )
	{
		// Locate the position of the block on the HIP device.
		for ( k = 0; k < hip_n_blocks; k++ )
			if ( args->hip_log[thread * hip_n_blocks + i].obj.base ==
					args->hip[thread * hip_n_blocks + k].obj.base )
				break;

		if ( k < hip_n_blocks )
		{
			// The block is now clean.
			args->hip[thread * hip_n_blocks + k].clean   = TRUE;
			args->hip[thread * hip_n_blocks + k].request = FALSE;
		}
	}

#ifdef FLA_ENABLE_MULTITHREADING
	FLA_Lock_release( &(args->hip_lock[thread]) ); // G ***
#endif

	return;
}

#endif // FLA_ENABLE_HIP

#ifdef FLA_ENABLE_MULTITHREADING

void FLASH_Queue_exec_parallel( void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_exec_parallel

	  ----------------------------------------------------------------------------*/
{

	//printf("ENTERING FLASH_Queue_exec_parallel\n");
	int   i;
	int   n_threads = FLASH_Queue_get_num_threads();
	void* (*thread_entry_point)( void* );

	// Allocate the thread structures array. Here, an array of FLASH_Thread
	// structures of length n_threads is allocated and the fields of each
	// structure set to appropriate values.
	FLASH_Thread* thread = ( FLASH_Thread* ) FLA_malloc( n_threads * sizeof( FLASH_Thread ) );

	// Initialize the thread structures array.
	for ( i = 0; i < n_threads; i++ )
	{
		// Save the thread's identifier.
		thread[i].id = i;

		// Save the pointer to the necessary variables with the thread.
		thread[i].args = arg;

		// The pthread object, if it was even compiled into the FLASH_Thread
		// structure, will be initialized by the pthread implementation when we
		// call pthread_create() and does not need to be touched at this time.
	}

	// Determine which function to send threads to.
	thread_entry_point = FLASH_Queue_exec_parallel_function;

#if FLA_MULTITHREADING_MODEL == FLA_OPENMP

	// An OpenMP parallel for region spawns n_threads threads. Each thread
	// executes the work function with a different FLASH_Thread argument.
	// An implicit synchronization point exists at the end of the curly
	// brace scope.
#pragma omp parallel for \
	private( i ) \
	shared( thread, n_threads, thread_entry_point ) \
	schedule( static, 1 ) \
	num_threads( n_threads )
	for ( i = 0; i < n_threads; ++i )
	{
		thread_entry_point( ( void* ) &thread[i] );
	}

#elif FLA_MULTITHREADING_MODEL == FLA_PTHREADS

	// Create each POSIX thread needed in addition to the main thread.
	for ( i = 1; i < n_threads; i++ )
	{
		int pthread_e_val;

		// Create thread i with default attributes.
		pthread_e_val = pthread_create( &(thread[i].pthread_obj),
				NULL,
				thread_entry_point,
				( void* ) &thread[i] );

#ifdef FLA_ENABLE_INTERNAL_ERROR_CHECKING
		FLA_Error e_val = FLA_Check_pthread_create_result( pthread_e_val );
		FLA_Check_error_code( e_val );
#endif
	}

	// The main thread is assigned the role of thread 0. Here we manually
	// execute it as a worker thread.
	thread_entry_point( ( void* ) &thread[0] );

	// Wait for non-main threads to finish.
	for ( i = 1; i < n_threads; i++ )
	{
		// These two variables are declared local to this for loop since this
		// is the only place they are needed, and since they would show up as
		// unused variables if FLA_MULTITHREADING_MODEL == FLA_PTHREADS.
		// Strangely, the Intel compiler produces code that results in an
		// "unaligned access" runtime message if thread_status is declared as
		// an int. Declaring it as a long or void* appears to force the
		// compiler (not surprisingly) into aligning it to an 8-byte boundary.
		int   pthread_e_val;
		void* thread_status;

		// Wait for thread i to invoke its respective pthread_exit().
		// The return value passed to pthread_exit() is provided to us
		// via status, if one was given.
		pthread_e_val = pthread_join( thread[i].pthread_obj,
				( void** ) &thread_status );

#ifdef FLA_ENABLE_INTERNAL_ERROR_CHECKING
		FLA_Error e_val = FLA_Check_pthread_join_result( pthread_e_val );
		FLA_Check_error_code( e_val );
#endif
	}

#endif

	FLA_free( thread );

	return;
}


//#include <sched.h>
//#include <sys/types.h>
//#include <linux/unistd.h>
//#include <errno.h>
//#include <unistd.h>
//#include <sys/syscall.h>


void* FLASH_Queue_exec_parallel_function( void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_exec_parallel_function

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args;   
	int           i;
	int           queue;
	int           cache;
	int           n_tasks   = FLASH_Queue_get_num_tasks();
	int           n_threads = FLASH_Queue_get_num_threads();
	int           n_cores   = FLASH_Queue_get_cores_per_cache();
	FLA_Bool      caching   = FLASH_Queue_get_caching();
	FLA_Bool      stealing  = FLASH_Queue_get_work_stealing();
	FLA_Bool      committed = TRUE;
	FLA_Bool      condition = TRUE;
	FLA_Bool      enabled   = FALSE;
	FLA_Bool      available;
	FLASH_Task*   t = NULL;
	FLASH_Task*   r = NULL;
	FLASH_Thread* me;
	//cpu_set_t     cpu_set;

	// Interpret the thread argument as what it really is--a pointer to an
	// FLASH_Thread structure.
	me = ( FLASH_Thread* ) arg;

	// Extract the variables from the current thread.
	args = ( FLASH_Queue_vars* ) me->args;

	// Figure out the id of the current thread.
	i = me->id;


	//debug
	//printf("Thread %d entering FLASH_Queue_exec_parallel_function\n", i);

	// Set the CPU affinity; We want the current thread i to run only on CPU i.
	//CPU_ZERO( &cpu_set );
	//CPU_SET( i, &cpu_set );
	//sched_setaffinity( syscall( __NR_gettid ), sizeof(cpu_set_t), &cpu_set );

	// Determine to which queue this thread belongs.
	queue = i / ( n_threads / args->n_queues );

	// Determine to which cache this thread belongs.
	cache = i / n_cores;

#ifdef FLA_ENABLE_GPU
	// Create memory on GPU.
	FLASH_Queue_create_gpu( i, ( void* ) args );

	// Save whether GPUs are enabled.
	enabled = FLASH_Queue_get_enabled_gpu();

	// Only use each GPU as its own cache when GPUs are enabled.
	if ( enabled )
		cache = i;
#endif
#ifdef FLA_ENABLE_HIP
	// Create memory in HIP.
#ifdef FLA_ENABLE_D2D

	if(args->pc == 0){

		FLASH_Queue_create_hip( i, ( void* ) args );

	}

#else
	FLASH_Queue_create_hip( i, ( void* ) args );
#endif
	// Save whether HIP is enabled.
	enabled = FLASH_Queue_get_enabled_hip();

	// Only use each accelerator as its own cache when HIP is enabled.
	if ( enabled )
		cache = i;
#endif

	// Prefetch blocks into the cache before execution.
	if ( caching && !enabled && i % n_cores == 0 )
		FLASH_Queue_prefetch( cache, ( void* ) args );

	// Loop until all the tasks have committed.
	while ( condition )
	{
#ifdef FLA_ENABLE_GPU
		// Check to see if any blocks on GPU need to be flushed.
		FLASH_Queue_flush_gpu( i, ( void* ) args );
#endif      
#ifdef FLA_ENABLE_HIP
		// Check to see if any blocks in HIP need to be flushed.
#ifdef FLA_ENABLE_D2D
		//do nothing if d2d is on
#else 
		FLASH_Queue_flush_hip( i, ( void* ) args );
#endif
#endif

		// Dequeue a task if there has not been one binded to thread.
		if ( r == NULL )
		{
			FLA_Lock_acquire( &(args->run_lock[queue]) ); // R ***

			// Obtain task to execute.
			t = FLASH_Queue_wait_dequeue( queue, cache, ( void* ) args );

			FLA_Lock_release( &(args->run_lock[queue]) ); // R ***
		}
		else
		{
			// Obtain the binded task.
			t = r;
			r = NULL;
		}

		// Dequeued a task from the waiting queue.
		available = ( t != NULL );

		//debug
		//printf("Thread %d available is %d\n", i, available );


		if ( available )
		{

			/*printf("******** Thread %d entering Task %s %d ********\n", i, t->name, t->order);
			for(int i = 0; i < t->n_input_args; i++){
				printf("input args %d has thread %c\n", i, t->input_arg[i].base->thread);
			}
			for(int i = 0; i < t->n_output_args; i++){
				printf("output args %d has thread %c\n", i, t->output_arg[i].base->thread);
			}	
			*/
			// Save the thread and cache that executes the task.
			t->thread = i;
			t->cache = cache;

			if ( caching && !enabled )
			{
				// Update the current state of the cache.
				FLASH_Queue_update_cache( t, ( void* ) args );
			}        

#ifdef FLA_ENABLE_GPU         
			// Execute the task on GPU.
			committed = FLASH_Queue_exec_gpu( t, ( void* ) args );

#elif defined(FLA_ENABLE_HIP)
			// Execute the task through HIP.
			//printf("Thread %d entering task %s\n", t->thread, t->name );
			//printf("Thread %d before FLASH_Queue_exec_gpu t->input thread: %d  t->output thread: %d\n", t->thread, t->input_arg[0].thread, t->output_arg[0].thread);
			committed = FLASH_Queue_exec_hip( t, ( void* ) args );
			//printf("Thread %d after FLASH_Queue_exec_gpu t->input thread: %d  t->output thread: %d\n", t->thread, t->input_arg[0].thread, t->output_arg[0].thread);
#else
			// Execute the task.
			FLASH_Queue_exec_task( t );
#endif

			// If the task has executed or not.
			if ( committed )
			{
				// Update task dependencies.
				r = FLASH_Task_update_dependencies( t, ( void* ) args );


				// Free the task once it executes in parallel.
				FLASH_Task_free_parallel( t, ( void* ) args );



			}
		}
		else
		{
			if ( stealing )
			{
				// Perform work stealing if there are no tasks to dequeue.
				r = FLASH_Queue_work_stealing( queue, ( void* ) args );
			}
		}

		FLA_Lock_acquire( &(args->all_lock) ); // A ***

		// Increment program counter.
		if ( available && committed )
			args->pc++;

		// Terminate loop.
		if ( args->pc >= n_tasks )
			condition = FALSE;

		FLA_Lock_release( &(args->all_lock) ); // A ***
	}

#ifdef FLA_ENABLE_GPU
	// Destroy and flush contents of GPU back to main memory.
	FLASH_Queue_destroy_gpu( i, ( void* ) args );
#endif

#ifdef FLA_ENABLE_HIP
<<<<<<< HEAD
#ifdef FLA_ENABLE_D2D
	//debug
	FLASH_Queue_destroy_hip( i, ( void* ) args );
	//FLASH_Queue_sync_hip( );


#else
	// Destroy and flush contents of accelerators back to main memory.
	FLASH_Queue_destroy_hip( i, ( void* ) args );
	FLASH_Queue_sync_hip( );
#endif
=======
   // Destroy and flush contents of accelerators back to main memory.
   FLASH_Queue_destroy_hip( i, ( void* ) args );
   FLASH_Queue_sync_hip( );
>>>>>>> bef93ebff3f450a947ce8ca22490dc5eeb952036
#endif

#if FLA_MULTITHREADING_MODEL == FLA_PTHREADS
	// If this is a non-main thread, then exit with a zero (normal) error code.
	// The main thread cannot call pthread_exit() because this routine never
	// returns. The main thread must proceed so it can oversee the joining of
	// the exited non-main pthreads.
	if ( i != 0 )
		pthread_exit( ( void* ) NULL );
#endif

	return ( void* ) NULL;
}


FLASH_Task* FLASH_Task_update_dependencies( FLASH_Task* t, void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Task_update_dependencies

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int         i;
	int         q = t->queue;
	int         queue;
	int         thread;
	int         n_threads = FLASH_Queue_get_num_threads();
	FLA_Bool    caching   = FLASH_Queue_get_caching();
	FLA_Bool    stealing  = FLASH_Queue_get_work_stealing();
	FLA_Bool    available;
	FLASH_Task* task;
	FLASH_Task* r = NULL;
	FLASH_Dep*  d = t->dep_arg_head;

	// Dequeue task to bind to thread if caching is enabled.
	if ( caching )
	{
		FLA_Lock_acquire( &(args->run_lock[q]) ); // R ***

		// Obtain task to execute.
		r = FLASH_Queue_wait_dequeue( q, t->cache, arg );

		FLA_Lock_release( &(args->run_lock[q]) ); // R ***
	}

	// Check each dependent task.
	for ( i = 0; i < t->n_dep_args; i++ )
	{
		if ( stealing )
		{
			// Place all dependent tasks onto same queue as predecessor task.
			d->task->queue = q;
		}

		task   = d->task;
		queue  = task->queue;
		thread = task->order % n_threads;

		FLA_Lock_acquire( &(args->dep_lock[thread]) ); // D ***

		task->n_ready--;
		available = ( task->n_ready == 0 );

		FLA_Lock_release( &(args->dep_lock[thread]) ); // D ***

		// Place newly ready tasks on waiting queue.      
		if ( available )
		{
			// If caching is enabled and the task belongs to this thread's queue.
			if ( caching && q == queue )
			{
				// Determine if there is a new binded task.
				r = FLASH_Task_update_binding( task, r, arg );
			}
			else
			{
				FLA_Lock_acquire( &(args->run_lock[queue]) ); // R ***

				FLASH_Queue_wait_enqueue( task, arg );

				FLA_Lock_release( &(args->run_lock[queue]) ); // R ***
			}
		}

		// Go to the next dep.
		d = d->next_dep;
	}

	return r;
}


FLASH_Task* FLASH_Task_update_binding( FLASH_Task* t, FLASH_Task* r, 
		void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Task_update_binding

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int queue;

	if ( r == NULL )
	{
		// There are no tasks on waiting queue, so bind the first task.
		r = t;
		r->hit = TRUE;
	}
	else
	{
		// Swap the binded task for the new ready task.
		if ( !r->hit || ( FLASH_Queue_get_sorting() && r->height < t->height ) )
		{
			queue = r->queue;
			r->hit = FALSE;

			FLA_Lock_acquire( &(args->run_lock[queue]) ); // R ***

			// Place swapped task back onto waiting queue.
			FLASH_Queue_wait_enqueue( r, arg );

			FLA_Lock_release( &(args->run_lock[queue]) ); // R ***

			// Bind the new ready task.
			r = t;
			r->hit = TRUE;
		}
		else // Keep the binded task and enqueue new ready task.
		{
			queue = t->queue;

			FLA_Lock_acquire( &(args->run_lock[queue]) ); // R ***

			FLASH_Queue_wait_enqueue( t, arg );

			FLA_Lock_release( &(args->run_lock[queue]) ); // R ***
		}
	}

	return r;
}


void FLASH_Task_free_parallel( FLASH_Task* t, void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Task_free_parallel

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;   
	int        i, j, k;
	int        thread;
	int        n_threads = FLASH_Queue_get_num_threads();
	FLASH_Dep* d;
	FLASH_Dep* next_dep;
	FLA_Obj    obj;

	// Clearing the last write task in each output block.
	for ( i = 0; i < t->n_output_args; i++ )
	{
		obj = t->output_arg[i];

		// Macroblock is used.
		if ( FLA_Obj_elemtype( obj ) == FLA_MATRIX )
		{
			dim_t    jj, kk;
			dim_t    m    = FLA_Obj_length( obj );
			dim_t    n    = FLA_Obj_width( obj );
			dim_t    cs   = FLA_Obj_col_stride( obj );
			FLA_Obj* buf  = FLASH_OBJ_PTR_AT( obj );

			// Clear each block in macroblock.
			for ( jj = 0; jj < n; jj++ )
				for ( kk = 0; kk < m; kk++ )
					( buf + jj * cs + kk )->base->write_task = NULL;
		}
		else // Clear regular block.
		{
			obj.base->write_task = NULL;
		}
	}

	// Cleaning the last read tasks in each input block.
	for ( i = 0; i < t->n_input_args; i++ )
	{
		obj = t->input_arg[i];

		// Macroblock is used.
		if ( FLA_Obj_elemtype( obj ) == FLA_MATRIX )
		{
			dim_t    jj, kk;
			dim_t    m    = FLA_Obj_length( obj );
			dim_t    n    = FLA_Obj_width( obj );
			dim_t    cs   = FLA_Obj_col_stride( obj );
			FLA_Obj* buf  = FLASH_OBJ_PTR_AT( obj );

			// Clear each block in macroblock.
			for ( jj = 0; jj < n; jj++ )
			{
				for ( kk = 0; kk < m; kk++ )
				{
					obj = *( buf + jj * cs + kk );

					thread = obj.base->n_read_blocks % n_threads;

					FLA_Lock_acquire( &(args->war_lock[thread]) ); // W ***

					k = obj.base->n_read_tasks;
					d = obj.base->read_task_head;

					obj.base->n_read_tasks   = 0;
					obj.base->read_task_head = NULL;
					obj.base->read_task_tail = NULL;

					FLA_Lock_release( &(args->war_lock[thread]) ); // W ***

					for ( j = 0; j < k; j++ )
					{
						next_dep = d->next_dep;
						FLA_free( d );
						d = next_dep;
					}
				}
			}
		}
		else // Regular block.
		{
			thread = obj.base->n_read_blocks % n_threads;

			FLA_Lock_acquire( &(args->war_lock[thread]) ); // W ***

			k = obj.base->n_read_tasks;
			d = obj.base->read_task_head;

			obj.base->n_read_tasks   = 0;
			obj.base->read_task_head = NULL;
			obj.base->read_task_tail = NULL;

			FLA_Lock_release( &(args->war_lock[thread]) ); // W ***

			for ( j = 0; j < k; j++ )
			{
				next_dep = d->next_dep;
				FLA_free( d );
				d = next_dep;
			}
		}
	}

	// Free the dep_arg field of t.
	d = t->dep_arg_head;

	for ( i = 0; i < t->n_dep_args; i++ )
	{
		next_dep = d->next_dep;
		FLA_free( d );
		d = next_dep;
	}

	// Free the int_arg field of t.
	FLA_free( t->int_arg );

	// Free the fla_arg field of t.
	FLA_free( t->fla_arg );

	// Free the input_arg field of t.
	FLA_free( t->input_arg );

	// Free the output_arg field of t.
	FLA_free( t->output_arg );

	// Finally, free the struct itself.
	FLA_free( t );

	return;
}

#endif


// ============================================================================


#ifndef FLA_ENABLE_MULTITHREADING

void FLASH_Queue_exec_simulation( void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_exec_simulation

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int           i, j;
	int           queue;
	int           cache;
	int           n_stages  = 0;
	int           n_queues  = args->n_queues;
	int           n_tasks   = FLASH_Queue_get_num_tasks();
	int           n_threads = FLASH_Queue_get_num_threads();
	int           n_cores   = FLASH_Queue_get_cores_per_cache();
	FLASH_Verbose verbose   = FLASH_Queue_get_verbose_output();
	FLASH_Task*   task;
	FLASH_Task*   t;
	FLASH_Dep*    d;

	// An array to hold tasks to be executed during of simulation.
#ifdef FLA_ENABLE_WINDOWS_BUILD
	FLASH_Task** exec_array = ( FLASH_Task** ) FLA_malloc( n_threads * sizeof( FLASH_Task* ) );
#else
	FLASH_Task* exec_array[n_threads];
#endif

	for ( i = 0; i < n_threads; i++ )
	{
		// Initialize all exec_array to NULL.
		exec_array[i] = NULL;

		// Prefetch blocks into the cache before execution.
		if ( i % n_cores == 0 )
			FLASH_Queue_prefetch( i, arg );
	}

	// Loop until all the tasks have committed.
	while ( args->pc < n_tasks )
	{
		for ( i = 0; i < n_threads; i++ )
		{
			// Update waiting queue with ready tasks.
			t = exec_array[i];

			if ( t != NULL )
			{
				// Check each dependent task.
				d = t->dep_arg_head;

				for ( j = 0; j < t->n_dep_args; j++ )
				{
					task = d->task;              
					task->n_ready--;

					// Place newly ready tasks on waiting queue.
					if ( task->n_ready == 0 )
					{
						FLASH_Queue_wait_enqueue( task, arg );
					}

					// Go to the next dep.
					d = d->next_dep;
				}

				// Free the task.
				FLASH_Task_free( t );
			}
		}      

		n_stages++;
		if ( !verbose )
			printf( "%7d", n_stages );

		// Move ready tasks from the waiting queue to execution queue.
		for ( i = 0; i < n_threads; i++ )
		{
			// Determine to which queue this thread belongs.
			queue = i / ( n_threads / n_queues );

			// Determine to which cache this thread belongs.
			cache = i / n_cores;

			// Dequeue a task.
			t = FLASH_Queue_wait_dequeue( queue, cache, arg );

			// Save the task for execution.
			exec_array[i] = t;

			if ( t != NULL )
			{
				// Save the thread and cache that executes the task.
				t->thread = i;
				t->cache = cache;

				// Increment program counter.
				args->pc++;
			}
		}

		// Execute independent tasks.
		for ( i = 0; i < n_threads; i++ )
		{
			t = exec_array[i];
			FLASH_Queue_update_cache( t, arg );
			FLASH_Queue_exec_task( t );

			if ( !verbose )
				printf( "%7s", ( t == NULL ? "     " : t->name ) );

			// Free the task if this is the last stage.
			if ( args->pc == n_tasks && t != NULL )
				FLASH_Task_free( t );
		}

		if ( !verbose ) 
			printf( "\n" );
	}

	if ( !verbose )
		printf( "\n" );

#ifdef FLA_ENABLE_WINDOWS_BUILD
	FLA_free( exec_array );
#endif

	return;
}

#endif

#else // FLA_ENABLE_SCC

int RCCE_acquire_lock(int);
int RCCE_release_lock(int);
double RCCE_wtime(void);
int    RCCE_ue(void);

//This function needs to be defined in the driver
// or linked in some how by the user.  
//It just just RCCE_barrier( &RCCE_COMM_WORLD ),
// but we can't implement it here because we're 
// trying not to link in RCCE.h
void Synch_all();


typedef struct FLASH_Queue_variables
{
	// Queue of all the tasks.
	FLASH_Task** task_queue;

	// The waiting queue of tasks for each thread.
	int*         n_ready;

	// The waiting queue of tasks for each thread.
	int*         wait_queue;

	// The number of tasks on waiting queue.
	int*         n_wait;

	// A global task counter that keeps track of how many tasks on the waiting
	// queue have been processed.
	int*         pc;
} FLASH_Queue_vars;


void FLASH_Queue_exec( void )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_exec

	  ----------------------------------------------------------------------------*/
{
	int         n_tasks    = FLASH_Queue_get_num_tasks();
	int         i;
	double      dtime;

	// All the necessary variables for the SuperMatrix mechanism.
	FLASH_Queue_vars args;

	// If the queue is empty, return early.
	if ( n_tasks == 0 )
		return;

	// Turn off all multiple queue implementations.
	FLASH_Queue_set_data_affinity( FLASH_QUEUE_AFFINITY_NONE );
	FLASH_Queue_set_work_stealing( FALSE );
	// Do not use cache affinity yet.
	FLASH_Queue_set_caching( FALSE );

	// Allocate memory for task queues.
	args.task_queue = ( FLASH_Task** ) FLA_malloc( n_tasks * sizeof( FLASH_Task* ) );
	args.n_ready = ( int* ) FLA_shmalloc( n_tasks * sizeof( int ) );
	args.wait_queue = ( int* ) FLA_shmalloc( n_tasks * sizeof( int ) );
	args.n_wait = ( int* ) FLA_shmalloc( sizeof( int ) );
	args.pc = ( int* ) FLA_shmalloc( sizeof( int ) );

	// Initialize data.
	if ( FLA_is_owner() )
	{
		args.n_wait[0] = 0;
		args.pc[0] = 0;
	}

	Synch_all();

	// Initialize tasks with critical information.
	FLASH_Queue_init_tasks( ( void* ) &args );

	// Display verbose output before free all tasks.
	if ( FLASH_Queue_get_verbose_output() )
		FLASH_Queue_verbose_output();

	// Start timing the parallel execution.
	dtime = RCCE_wtime();

	FLASH_Queue_exec_parallel_function( ( void* ) &args );

	// End timing the parallel execution.
	dtime = RCCE_wtime() - dtime;
	FLASH_Queue_set_parallel_time( dtime );

	// Free all tasks sequentially.
	for ( i = 0; i < n_tasks; i++ )
		FLASH_Task_free( args.task_queue[i] );

	// Free data.
	FLA_free( args.task_queue );
	FLA_shfree( args.n_ready );
	FLA_shfree( args.wait_queue );
	FLA_shfree( args.n_wait );
	FLA_shfree( args.pc );

	// Reset values for next call to FLASH_Queue_exec().
	FLASH_Queue_reset();

	return;
}


void FLASH_Queue_init_tasks( void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_init_tasks

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int            i, j;
	int            n_tasks = FLASH_Queue_get_num_tasks();
	int            n_ready = 0;
	int            height;
	FLASH_Task*    t;
	FLASH_Dep*     d;

	// Grab the tail of the task queue.
	t = FLASH_Queue_get_tail_task();

	for ( i = n_tasks - 1; i >= 0; i-- )
	{
		// Save all the task pointers.
		args->task_queue[i] = t;

		// Only use a single queue implementation.
		t->queue = 0;

		// Determine the height of each task in the DAG.
		height = 0;
		d = t->dep_arg_head;

		// Take the maximum height of dependent tasks.
		for ( j = 0; j < t->n_dep_args; j++ )
		{
			height = max( height, d->task->height );
			d = d->next_dep;
		}

		t->height = height + 1;

		// Since freeing a task is always a leaf, we want to force it to execute 
		// earlier by giving it a greater height in order to reclaim memory.
		if ( t->func == (void *) FLA_Obj_free_buffer_task )
			t->height += n_tasks;

		// Find all ready tasks.
		t->n_ready += t->n_input_args + t->n_output_args + 
			t->n_macro_args + t->n_war_args;

		if ( t->n_ready == 0 )
		{
			// Save the number of ready and available tasks.
			n_ready++;
		}

		if ( FLA_is_owner() )
		{
			// Record all the ready values.
			args->n_ready[i] = t->n_ready;
		}

		// Go to the previous task.
		t = t->prev_task;
	}

	// Only allow the first core to enqueue the initial ready tasks.
	if ( !FLA_is_owner() )
		return;

	// Grab the head of the task queue.
	t = FLASH_Queue_get_head_task();

	for ( i = 0; i < n_tasks && n_ready > 0; i++ )
	{
		if ( t->n_ready == 0 )
		{
			RCCE_acquire_lock( 0 );

			// Enqueue all the ready and available tasks.
			FLASH_Queue_wait_enqueue( t, arg );

			RCCE_release_lock( 0 );

			// Decrement the number of ready tasks left to be enqueued.
			n_ready--;
		}

		// Go to the next task.
		t = t->next_task;
	}

	return;
}


void FLASH_Queue_wait_enqueue( FLASH_Task* t, void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_wait_enqueue

	  ----------------------------------------------------------------------------*/
{ 

	//debug
	//printf("Thread %d entering FLASH_Queue_wait_enqueue\n", t->thread); 
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int i = args->n_wait[0] + args->pc[0];

	// Insertion sort of tasks in waiting queue.
	if ( FLASH_Queue_get_sorting() )
	{
		for ( ; i > args->pc[0]; i-- )
		{
			if ( args->task_queue[args->wait_queue[i-1]]->height >
					args->task_queue[t->order]->height )
				break;

			args->wait_queue[i] = args->wait_queue[i-1];
		}
	}

	args->wait_queue[i] = t->order;

	// Increment number of tasks on waiting queue.
	args->n_wait[0]++;

	return;
}


FLASH_Task* FLASH_Queue_wait_dequeue( int queue, int cache, void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_wait_dequeue

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	FLASH_Task* t = NULL;

	if ( args->n_wait[0] > 0 )
	{
		// Grab the head of the queue.
		t = args->task_queue[args->wait_queue[args->pc[0]]];

		// Decrement number of tasks on waiting queue.
		args->n_wait[0]--;

		// Increment the program counter.
		args->pc[0]++;
	}

	return t;
}


void* FLASH_Queue_exec_parallel_function( void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Queue_exec_parallel_function

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int           i         = RCCE_ue();
	int           queue     = 0;
	int           cache     = 0;
	int           n_tasks   = FLASH_Queue_get_num_tasks();
	int           n_threads = FLASH_Queue_get_num_threads();
	FLA_Bool      condition;
	FLA_Bool      available;
	FLASH_Task*   t = NULL;

	// Do not let extraneous cores execute.
	if ( i < n_threads )
		condition = TRUE;
	else
		condition = FALSE;

	// Loop until all the tasks have committed.
	while ( condition )
	{
		RCCE_acquire_lock( 0 );

		// Obtain task to execute.
		t = FLASH_Queue_wait_dequeue( queue, cache, ( void* ) args );

		RCCE_release_lock( 0 );

		// Dequeued a task from the waiting queue.
		available = ( t != NULL );

		if ( available )
		{
			// Save the thread and cache that executes the task.
			t->thread = i;
			t->cache = cache;

			// Execute the task.
			FLASH_Queue_exec_task( t );

			// Update task dependencies.
			FLASH_Task_update_dependencies( t, ( void* ) args );
		}

		RCCE_acquire_lock( 0 );

		// Terminate loop.
		if ( args->pc[0] >= n_tasks )
			condition = FALSE;

		RCCE_release_lock( 0 );
	}

	return ( void* ) NULL;
}


FLASH_Task* FLASH_Task_update_dependencies( FLASH_Task* t, void* arg )
	/*----------------------------------------------------------------------------

	  FLASH_Task_update_dependencies

	  ----------------------------------------------------------------------------*/
{
	FLASH_Queue_vars* args = ( FLASH_Queue_vars* ) arg;
	int         i;
	int         n_threads = FLASH_Queue_get_num_threads();
	int         thread;
	FLA_Bool    available;
	FLASH_Task* task;
	FLASH_Task* r = NULL;
	FLASH_Dep*  d = t->dep_arg_head;

	// Check each dependent task.
	for ( i = 0; i < t->n_dep_args; i++ )
	{
		task = d->task;

		// Use the remaining locks except for the first one.
		thread = ( n_threads > 1 ? task->order % ( n_threads - 1 ) + 1 : 0 );

		RCCE_acquire_lock( thread );

		args->n_ready[task->order]--;
		available = ( args->n_ready[task->order] == 0 );

		RCCE_release_lock( thread );

		// Place newly ready tasks on waiting queue.      
		if ( available )
		{
			RCCE_acquire_lock( 0 );

			FLASH_Queue_wait_enqueue( task, arg );

			RCCE_release_lock( 0 );
		}

		// Go to the next dep.
		d = d->next_dep;
	}

	return r;
}

#endif // FLA_ENABLE_SCC

#endif // FLA_ENABLE_SUPERMATRIX
