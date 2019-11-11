#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/syslog.h>
#include <linux/sched.h>
#include <linux/slab.h> // Include kfree
#include <linux/mutex.h>
#include <linux/time.h>

static ulong num_threads = 1;
static ulong upper_bound = 10;

module_param(num_threads, ulong, 0);
module_param(upper_bound, ulong, 0);

static int prime_bound;
static struct task_struct **threads; //array of task_struct pointers
int *thread_track; //ptr to an array of counter variables where each thread will keep track of how many times it has "crossed out" a non-prime number
atomic_t *primes_computed; // a pointer to the array of integers, within which primes will be computed


int current_position; // position of the current number (prime in the single-threaded version) that is being processed (each thread has own)
int num_threads_created;
atomic_t threads_locked[2]; //number of threads that have been locked for each barrier sync
atomic_t all_threads_finish; // whether the computation of prime numbers has finished in each thread
atomic_t threads_at_b1 = ATOMIC_INIT(0);
atomic_t threads_at_b2 = ATOMIC_INIT(0);

struct timespec time_module_init; // module initialization begins
struct timespec time_begin; // all threads at first barrier about to begin work
struct timespec time_complete; // all threads at second barrier completed work

static int barrier_sync(ulong barrier_num) {
	atomic_inc(&threads_locked[barrier_num]);
	while(atomic_read(&threads_locked[barrier_num]) < num_threads_created){}
	return 0;
}
static void remove_primes(int prime_index, ulong thread_index){
	int i;
	int prime;
	prime = atomic_read(primes_computed + prime_index);
	if(prime == 0){
		return;
	}
	for(i = 2 * prime_index; i < prime_bound; i += prime){
		atomic_set(primes_computed + i, 0);
		thread_track[thread_index]++;
	}
}

static void compute_primes(ulong thread_index){
	int i;
	while(current_position < prime_bound){
		i = current_position;
		current_position++;
		while(current_position < prime_bound && atomic_read(primes_computed + current_position) == 0){
			current_position++;
		}

		remove_primes(i, thread_index);
	}
}

static int spawned_call(void* data) {
	ulong thread_index = (ulong) data;
	if(!kthread_should_stop()){
		set_current_state(TASK_INTERRUPTIBLE);

		atomic_inc(&threads_at_b1);
		if(atomic_read(&threads_at_b1) == num_threads_created){
			getnstimeofday(&time_begin);
		}
		barrier_sync(0);

		compute_primes(thread_index);

		atomic_inc(&threads_at_b2);
		if(atomic_read(&threads_at_b2) == num_threads_created){
			getnstimeofday(&time_complete);
		}
		barrier_sync(1);

		atomic_inc(&all_threads_finish);
		schedule();
	}
	return 0;
}

static int __init lab02_module_init(void) {
	getnstimeofday(&time_module_init);
	if(num_threads > 0 && upper_bound > 1){
		ulong i;
		prime_bound = (int) (upper_bound + 1);

		primes_computed = (atomic_t*) kmalloc(prime_bound * sizeof(atomic_t),  GFP_KERNEL);
		if(primes_computed == NULL){
			printk("There was not enough kernel memory to store numbers up to upper_bound");
			return 0;
		}

		thread_track = (int*) kmalloc(num_threads * sizeof(int),  GFP_KERNEL);
		if(thread_track == NULL){
			printk("There was not enough kernel memory to store a counter for each thread");
			return 0;
		}

		threads = (struct task_struct**) kmalloc(num_threads * sizeof(struct task_struct*), GFP_KERNEL);
		if (threads == NULL){
			printk("There was not enough kernel memory to store a task_struct for each thread.");
			return 0;
		}

		atomic_set(threads_locked, 0);
		atomic_set(threads_locked + 1, 0);
		atomic_set(primes_computed, 0);
		atomic_set(primes_computed + 1, 0);
		for(i = 2; i < prime_bound; ++i){
			atomic_set(primes_computed + i, (int)i);
		}

		atomic_set(&all_threads_finish, 0);
		current_position = 0; // Position of value 2
		for(i = 0; i < num_threads; ++i){
			thread_track[i] = 0;
			threads[i] = kthread_create(spawned_call,(void*) i, "Lab2Thread");
			if(threads[i]){
				num_threads_created++;
			}
		}
		for(i = 0; i < num_threads; ++i){
			if(threads[i]){
				wake_up_process(threads[i]);
			}
			else{
				printk("Thread %lu could not be woken.", i);
			}
		}
	} else { // invalid parameters: explain what is wrong and return
    if (num_threads <= 0) {
      if (upper_bound <= 1) {
        printk("Invalid parameters:<num_threads> <upper_bound>:<%lu> <%lu>: num_threads must be 1 or greater, upper bound must be 2 or greater", num_threads, upper_bound);
        return 0;
      }
      printk("Invalid parameters:<num_threads> <upper_bound>:<%lu> <%lu>: num_threads must be 1 or greater", num_threads, upper_bound);
      return 0;
    }
		printk("Invalid parameters:<num_threads> <upper_bound>:<%lu> <%lu>: upper bound must be 2 or greater", num_threads, upper_bound);
    return 0;
	}
	return 0;
}

static void __exit lab02_module_exit(void) {
	if(threads == NULL || thread_track == NULL || primes_computed == NULL){
		return;
	}
	if(num_threads > 0 && upper_bound > 1){
		if(atomic_read(&all_threads_finish) == num_threads_created){
			ulong i;
			ulong j;
			int setup_time = 0;
			int process_time = 0;
			int threads_marked = 0;
			int num_primes = 0;
			int num_composites = -2; // Subtract 2 for index 0 and index 1
			for(i = 0; i < num_threads; ++i){
				threads_marked += thread_track[i];
				if(threads[i]){
					kthread_stop(threads[i]);
				}
			}
			printk("Primes:\n ");
			i = 0;
			while(i < prime_bound){
				j = 0;
				while(j < 8){
					if(atomic_read(primes_computed + i) == 0){
						++num_composites;
					}
					else{
						printk(KERN_CONT "%d ", atomic_read(primes_computed + i));
						++num_primes;
						++j;	
					}
					++i;
					if(i >= prime_bound){
						break;
					}
				}
				printk(" ");
			}
			printk("%d prime numbers were found. %d composite numbers were found. %d numbers were unneccesarily crossed out.",
					num_primes, num_composites, threads_marked - num_composites
			);
			printk("Module parameters: num_threads: %lu, upper_bound: %lu", num_threads, upper_bound);

			setup_time += (int) 1e9 * ((int) time_begin.tv_sec - (int) time_module_init.tv_sec);
			setup_time += (int) time_begin.tv_nsec - (int) time_module_init.tv_nsec;
			printk("Module setup time: %d nanoseconds", setup_time);

			process_time += (int) 1e9 * ((int) (time_complete.tv_sec) - (int) (time_begin.tv_sec));
			process_time += (int) (time_complete.tv_nsec - (int) time_begin.tv_nsec);
			printk("Prime processing time: %d nanoseconds", process_time);

			if(thread_track){
				kfree(thread_track);
			}
			if(primes_computed){
				kfree(primes_computed);
			}
			if(threads){
				kfree(threads);
			}
			printk("Module exited safely");
		}
		else{
			printk("The threads have not finished executing. Exiting with memory leaks.");
		}
	}
	else{
		printk("bad kernel module parameters");
	}
}

module_init(lab02_module_init);
module_exit(lab02_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Elaine Cole, Jesse Huang, Zhengliang Liu");
MODULE_DESCRIPTION("Atomic Sieve Module");
