/*
   This file is part of Wool, a library for fine-grained independent 
   task parallelism

   Copyright (C) 2009- Karl-Filip Faxen
      kff@sics.se

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

#include <sched.h> // For improved multiprocessor performance
#include <time.h>  // d:o
#include "wool.h"
#include <stdlib.h>
#include <stdio.h>
#ifndef __APPLE__
#include <malloc.h>
#endif
#include <string.h>
#include <unistd.h>

#if 1 || LOG_EVENTS
#include <time.h>
#endif

/* Implements 
   leapfrogging, 
   private tasks (fixed depth),
   peek (check stealable before locking),
   trylock (steal from other worker if other thief busy),
   linear stealing (rather than always random)
   yielding after unsuccessful stealing
   sleeping after more unsuccessful stealing

   Does not implement
   Resizing or overflow checking of task pool
*/

#define SO_STOLE 0
#define SO_BUSY  1
#define SO_NO_WORK 2
#define SO_THIEF 3

#ifndef WOOL_SYNC_NOLOCK
  #define WOOL_SYNC_NOLOCK 1
#endif

#ifndef WOOL_STEAL_NOLOCK
  #define WOOL_STEAL_NOLOCK (! THE_SYNC )
#endif

#ifndef WOOL_STEAL_SKIP
  #define WOOL_STEAL_SKIP 0 
#endif

#ifndef STEAL_TRYLOCK
  #define STEAL_TRYLOCK 1
#endif

#ifndef WOOL_STEAL_SET
  #define WOOL_STEAL_SET 1
#endif

#ifndef EXACT_STEAL_OUTCOME
  #define EXACT_STEAL_OUTCOME 0
#endif

#ifndef STEAL_PEEK
  #define STEAL_PEEK 1
#endif

#ifndef INIT_WORKER_DQ_SIZE
  #define INIT_WORKER_DQ_SIZE 1000
#endif

#ifdef __APPLE__
#define memalign(ALIGN, SIZE)  valloc(SIZE)
#endif

WOOL_WHEN_MSPAN( hrtime_t __wool_sc = 1000; )

static Worker **workers;
static int init_worker_dq_size = INIT_WORKER_DQ_SIZE;
int n_workers;
static int backoff_mode = 640; // No of iterations of waiting after 
static int n_stealable=0;
static int global_segment_size = 10;
static int global_refresh_interval = 100;
static int global_thieves_per_victim_x10 = 30;

static int steal( int, int, Task *, int );

static volatile int more_work = 1;
static pthread_t *ts = NULL;

#if SYNC_MORE
static wool_lock_t more_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static wool_cond_t sleep_cond;
static wool_lock_t sleep_lock;
static int old_thieves = 0, max_old_thieves = -1;

static pthread_attr_t worker_attr;

static int lock_delay = 10;

static int event_mask = -1;

#if WOOL_MEASURE_SPAN || LOG_EVENTS

// real time as a 64 bit unsigned
static hrtime_t gethrtime()
{
  struct timespec t;
  clock_gettime( CLOCK_REALTIME, &t );
  return 1000000000LL * t.tv_sec + t.tv_nsec;
}

#endif

#if WOOL_MEASURE_SPAN

static hrtime_t last_span, last_time, first_time;

static overhead = 90;

hrtime_t __wool_update_time( void )
{

  hrtime_t now       = gethrtime();
  hrtime_t this_time = now - last_time;

  last_span += this_time < overhead ? 0 : this_time - overhead;
  last_time = now;

  first_time += overhead;

  return last_span;
}

void __wool_set_span( hrtime_t span )
{
  last_span = span;
}

#endif

#if LOG_EVENTS

static LogEntry *logbuff[100];

static volatile hrtime_t diff[100], trip[100], first_time;

void logEvent( Worker *self, int what )
{
  LogEntry *p = self->logptr; 
  int event_class = what < 100 ? what : 24 + (what-100)/1024;

  if( ( event_mask & (1<<event_class) ) == 0 ) {
    return;
  }
  p->what = what;
  p->time = gethrtime( );

  self->logptr ++;
}

static void sync_clocks( Worker *self )
{
  hrtime_t slave_time=0;
  int i,k;

  for( i=0; i<2; i++ ) {
    while( self->clock != 1 ) ;
    self->clock = 2;
    for( k=3; k<8; k+=2 ) {
      while( self->clock != k ) ;
      if( k==5 ) slave_time = gethrtime();
      self->clock = k+1;
    }
  }
  while( self->clock != 9 ) ;
  self->time = slave_time;
  COMPILER_FENCE;
  self->clock = 10;
}



static void master_sync( )
{
  int i,j,k;
  hrtime_t master_time, round_trip;

  for( i=1; i<n_workers; i++ ) {
    Worker *slave = workers[i];
    for( j=0; j<2; j++ ) {
      slave->clock = 1;
      while( slave->clock != 2 ) ;
      master_time = gethrtime( );
      for( k=3; k<8; k+=2 ) {
        slave->clock = k;
        while( slave->clock != k+1 ) ;
      }
      round_trip = gethrtime() - master_time;
    }
    slave->clock = 9;
    while( slave->clock != 10 ) ;
    diff[i] = master_time + round_trip/2 - slave->time;
    trip[i] = round_trip;
  }
  diff[0] = 0;
  trip[0] = 0;
}
      
#endif

static wool_lock_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static wool_cond_t init_cond = PTHREAD_COND_INITIALIZER;
static int n_initialized = 0;

static void wait_for_init_done(void)
{
  wool_lock( &init_lock );
  n_initialized++;
  if( n_initialized == n_workers ) {
    wool_unlock( &init_lock );
    wool_broadcast( &init_cond );
  } else {
    wool_wait( &init_cond, &init_lock );
    wool_unlock( &init_lock );
  }
}

int yield_interval = 16; // Also set from command line
int sleep_interval = 100000; // Wait after so many attempts, also set by '-i'

// Decrement old thieves when an old thief successfully 
// steals but before the call. Since old_thieves is always <= max_old_thieves,
// one old thief is freed from jail (pardoned).
// A worker becomes an old thief after old_thief_age unsuccessful steal
// attempts. If there are already max_old_thieves, it goes to sleep on
// the jail. 

static int new_old_thief( void ) 
{
  int is_old, first = 1;



  // return 0;

  wool_lock( &sleep_lock );
  if( old_thieves >= max_old_thieves + 2 ) {
    // fprintf( stderr, "-\n" );
    while( more_work && old_thieves >= max_old_thieves + 2 ) {
      if( first ) {
        // fprintf( stderr, "s\n" );
        first = 0;
      }
      wool_wait( &sleep_cond, &sleep_lock );
    }
    // fprintf( stderr, "w\n" );
    is_old = 0;
  } else {
    // fprintf( stderr, "+\n" );
    old_thieves++;
    is_old = 1;
  }
  wool_unlock( &sleep_lock );

  return is_old;
}

static void decrement_old_thieves(void)
{
  int new_old;

  // return;

  // fprintf( stderr, "-\n" );

  wool_lock( &sleep_lock );
  old_thieves--;
  new_old = old_thieves;
  wool_unlock( &sleep_lock );
  if( new_old < max_old_thieves ) {
    // fprintf( stderr, "!\n" );
    wool_signal( &sleep_cond );
  }
}

balarm_t sync_get_balarm( Task *t )
{
  Worker   *self;
  balarm_t  a;

  self = get_self( t );
  wool_lock( self->dq_lock );
    a = t->balarm;
  wool_unlock( self->dq_lock );
  PR_INC( self, CTR_sync_lock );
  return a;
}

static int spin( Worker *self, int n )
{
  int i,s=0;

  // This code should hopefully confuse every optimizer. 
  for( i=1; i<=n; i++ ) {
    s |= i;
  }
  if( s > 0 ) {
    PR_ADD( self, CTR_spins, n );
  }
  return s&1;
}

void wool_sync( volatile Task *t, balarm_t a )
{
  volatile Worker *self = get_self( t );

#if ! WOOL_SYNC_NOLOCK
  wool_lock( self->dq_lock );
#endif
#if ! THE_SYNC
    while( a == NOT_STOLEN ) a = t->balarm; // Thief might not yet have written
#endif
    if( a == STOLEN_DONE || t->balarm == STOLEN_DONE ) {

      /* Stolen and completed */
      PR_INC( self, CTR_read );
      /* Do nothing */

    } else if( a > B_LAST ) {

      /* Stolen and in progress; let's leapfrog! */
      int done = 0;
      // Worker *thief = a;
      int thief_idx = a;
      // volatile int v;
      const int ny = 1;

#if ! WOOL_SYNC_NOLOCK
      wool_unlock( self->dq_lock ); 
#endif
      PR_INC( self, CTR_waits ); // It isn't waiting any more, though ...

      /* Now leapfrog */

      logEvent( (Worker *) self, 3 );
      #if WOOL_STEAL_SET
        self->is_thief = 1;
      #endif

      do {
        int steal_outcome = SO_NO_WORK;
        int yield_timer = ny*yield_interval;

        if( yield_timer-- == 0 ) {
          sched_yield( );
          yield_timer = ny*yield_interval;
        }

        PR_INC( self, CTR_leap_tries );
        steal_outcome = steal( self->idx, thief_idx, (Task *) t + 1, 0 );
        if( steal_outcome != SO_BUSY ) {
           PR_INC( self, CTR_leap_locks );
        }
        if( steal_outcome == SO_STOLE ) {
          PR_INC( self, CTR_leaps );
          yield_timer = ny*yield_interval;
        } else {
          // v = spin( self, backoff_mode );
        }
        if( t->balarm == STOLEN_DONE ) { // Leapfrogging is over!
          done = 1;
        }
      } while( !done );
      #if WOOL_STEAL_SET
        self->is_thief = 0;
      #endif
      logEvent( (Worker *) self, 4 );
#if ! WOOL_SYNC_NOLOCK && ! WOOL_STEAL_NOLOCK
      wool_lock( self->dq_lock );
#endif

    } else {
      fprintf( stderr, "Unknown task state %lu in sync\n", (unsigned long) a );
      exit( 1 );
    }

#if WOOL_SYNC_NOLOCK && ! WOOL_STEAL_NOLOCK
    wool_lock( self->dq_lock );
#endif

#if ! WOOL_DEFER_BOT_DEC
    if( ! WOOL_STEAL_NOLOCK || self->dq_bot > t ) {
      self->dq_bot = (Task *) t;
    } else {
      PR_INC( self, CTR_sync_no_dec );
    }
#else
    self->decrement_deferred = 1;
#endif

#if !WOOL_DEFER_NOT_STOLEN
    t->balarm = NOT_STOLEN;
#endif

#if ! WOOL_STEAL_NOLOCK
    wool_unlock( self->dq_lock );
#endif

}

struct _WorkerData {
  Worker w;
  Task   p[];
};

static void init_worker( int w_idx )
{
  int i;
  Worker *w;
  struct _WorkerData *d;
  int offset = w_idx * (sizeof(Worker) + 2*sizeof(Task));
  int dq_size = init_worker_dq_size * sizeof(Task);

  // We're offsetting the worker data a bit to avoid cache conflicts
  d = (struct _WorkerData *) 
      ( ( (char *) valloc( sizeof(Worker) + dq_size + offset ) ) + offset );
  w = &(d->w);

  w->dq_size = init_worker_dq_size;
  w->dq_base = &(d->p[0]);
  for( i=0; i < w->dq_size; i++ ) {
    w->dq_base[i].f = T_BUSY;
    w->dq_base[i].balarm = NOT_STOLEN;
    w->dq_base[i].stealable = i < n_stealable ? 1 : 0;
    w->dq_base[i].self = w;
  }
  w->dq_bot = w->dq_base;
  w->dq_top = w->dq_base; // Not used, really
  w->dq_lock = &( w->the_lock );
  pthread_mutex_init( w->dq_lock, NULL );
  for( i=0; i < CTR_MAX; i++ ) {
    w->ctr[i] = 0;
  }
  w->idx = w_idx;
  w->clock = 0;
  w->time = 0;
  w->decrement_deferred = 0;
  #if LOG_EVENTS
    logbuff[w->idx] = (LogEntry *) malloc( 1000000 * sizeof( LogEntry ) );
    w->logptr = logbuff[w->idx];
  #endif
  w->is_thief = w_idx == 0 ? 0 : 1;

  workers[w_idx] = w;
}


static int steal( int self_idx, int victim_idx, 
		  Task *dq_top, int is_old_thief )
{
  volatile Task *tp, *ep;
  void (*f) (Task *, Task *) = T_BUSY;
#if 1 || WOOL_STEAL_SET || LOG_EVENTS || COUNT_EVENTS
  Worker *self = workers[self_idx];
#endif

  volatile Worker *victim = workers[victim_idx];
  int skips = 0;

  logEvent( self, 100 + victim_idx );

#if STEAL_PEEK
  tp = victim->dq_bot;

#if WOOL_STEAL_SET
  if( victim->is_thief ) {
    return SO_THIEF;
  }
#endif

#if EXACT_STEAL_OUTCOME
  if( tp->f <= T_LAST || !(tp->stealable) ) {
    return SO_NO_WORK;
  } else {
    if( tp->balarm != NOT_STOLEN ) {
      if( tp[1].f <= T_LAST || !(tp[1].stealable) ) {
        return SO_NO_WORK;
      } else {
        return SO_BUSY;
      }
    }
  }
#else
  if( tp->balarm != NOT_STOLEN || tp->f <= T_LAST || !(tp->stealable) ) {
    return SO_NO_WORK;
  }
#endif
#else
  tp = victim->dq_bot;
#if WOOL_STEAL_SET
  if( victim->is_thief ) {
    return SO_THIEF;
  }
#endif

  if( ! (tp->stealable) ) {
    return SO_NO_WORK;
  }
#endif
#if !WOOL_STEAL_NOLOCK
  PREFETCH( tp->balarm ); // Start getting exclusive access

#if STEAL_TRYLOCK
  if( wool_trylock( victim->dq_lock ) != 0 ) {
#if EXACT_STEAL_OUTCOME
    if( tp[1].f <= T_LAST || !(tp[1].stealable) ) {
      return SO_NO_WORK;
    } else {
      return SO_BUSY;
    }
#else
    return SO_BUSY;
#endif
  }
#else
  wool_lock( victim->dq_lock );
#endif
  // now locked!
  // but only if we use locks!

    tp = victim->dq_bot;  // Yes, we need to reread after aquiring lock!

#endif

    __builtin_prefetch( (void *) tp, 1 );
    __builtin_prefetch( (void *) self, 1 );

    // The victim might have sync'ed or somebody else might have stolen
    // while we were obtaining the lock;
    // no point in getting exclusive access in that case.
    if( WOOL_STEAL_NOLOCK || 
        ( tp->stealable && tp->balarm == NOT_STOLEN && tp->f > T_LAST  ) ) { 
#if THE_SYNC
      // THE version
      tp->balarm = self_idx;
      MFENCE;
      f = tp->f;

      if( f > T_LAST ) {  // Check again after the fence!
        victim->dq_bot ++;
      } else {
        tp->balarm = NOT_STOLEN;
        tp = NULL;
      }
#else
      // Exchange version

      do {
        EXCHANGE( f, tp->f );

        if( f > T_LAST ) {
          int all_stolen = 1;

          tp->balarm = self_idx;

          for( ep = victim->dq_bot; ep < tp; ep++ ) {
            if( ep->f > T_LAST ) {
              all_stolen = 0;
            }
          }
          if( all_stolen ) {
            victim->dq_bot = (Task *) tp+1;
          } else {
            PR_INC( self, CTR_steal_no_inc );
          }
          if( skips != 0 ) {
            PR_INC( self, CTR_skip );
          }
          skips = 0;
        } else {
          if( WOOL_STEAL_SKIP && skips < 1 && tp[1].f > T_LAST && tp[1].stealable ) {
            while( victim->dq_bot == tp ) ; // spin( self, 2000 );
            tp++;
            skips++;
            PR_INC( self, CTR_skip_try );
          } else {
            tp = NULL;
          }
        }
      } while( skips != 0 && tp != NULL );

#endif

    } else {
#if EXACT_STEAL_OUTCOME
      if( tp->balarm != NOT_STOLEN ) {
        if( tp[1].f > T_LAST && tp[1].stealable ) {
          return SO_BUSY;
        }
      }
#endif
      tp = NULL; // Already stolen by someone else
    }
#if !WOOL_STEAL_NOLOCK
  wool_unlock( victim->dq_lock );
#endif

  if( tp != NULL ) {

    // One less old thief?
    if( is_old_thief ) {
      decrement_old_thieves( );
    }

    #if WOOL_STEAL_SET
      self->is_thief = 0;
    #endif

    logEvent( self, 1 );

    f( dq_top, (Task *) tp ); 

    logEvent( self, 2 );

    #if WOOL_STEAL_SET
      self->is_thief = 1;
    #endif

      // instead of locking, so that the return value is really updated
      // the return value is already written by the wrapper (f)

      STORE_INT_REL( &(tp->balarm), STOLEN_DONE );

    return SO_STOLE;
  }
  return SO_NO_WORK;
}

static void *do_work( void * );

static int myrand( unsigned int *seedp, int max )
{
  return rand_r( seedp ) % max;
}

int rand_interval = 40; // By default, scan sequentially for 0..39 attempts

#define min(a,b) ( (a)>(b) ? (b) : (a) )
#define max(a,b) ( (a)<(b) ? (b) : (a) )

#if WOOL_STEAL_SET

typedef short int vidx_t;

static void swap( vidx_t *v, int sw1, int sw2 )
{
  vidx_t tmp = v[sw1];
  v[sw1] = v[sw2];
  v[sw2] = tmp;
}

static void *do_work( void *arg )
{
  Worker **self_p = (Worker **) arg;
  Worker *self;
  unsigned int self_idx = self_p - workers;
  // unsigned int victim_idx = self_idx;
  unsigned int seed = self_idx;
  unsigned int n = n_workers;
  int i=0;
  // int local_sleep_interval = sleep_interval;
  // int attempts = 0, next_yield = yield_interval, next_sleep = local_sleep_interval;
  // int is_old_thief = 0;
  // int next_spin = n-1;
  volatile int v;
  int segment_size = global_segment_size;
  int vset_refresh_interval = global_refresh_interval;
  int thieves_per_victim_x10 = global_thieves_per_victim_x10;
  vidx_t* victim_set = (vidx_t *) malloc( (n-1) * sizeof(vidx_t) );
  int steal_outcome = SO_NO_WORK;

  init_worker( self_idx );
  wait_for_init_done();

  self = workers[self_idx];

  #if LOG_EVENTS
    sync_clocks( self );
  #endif

  for( i=0; i < n-1; i++ ) {
    victim_set[i] = (vidx_t) ( (self_idx+i+1) % n );
  }

  do { // Loop over successful steals
    do { // Loop over victim set refreshes

      // Refresh victim set

      int vset_current_size = 0;
      int vset_target_size = 3;
      int vset_refresh_timer = vset_refresh_interval;

      do { // Loop over multiple uses of the same victim set

        int segment_start = 0;
        int segment_end;
        int thieves_found = 0;
        int thief_est_x10;

        // Back off a little

        v = spin( self, myrand( &seed, 100 ) );

        // Grow or shrink victim set
        vset_target_size = min( vset_target_size, 2*vset_current_size );
        vset_target_size = max( vset_target_size, vset_current_size/2);
        vset_target_size = max( vset_target_size, 3 );
        vset_target_size = min( vset_target_size, n-1 );

        if( vset_target_size > vset_current_size ) {
          // Grow victim set
          for( i = vset_current_size; i < vset_target_size; i++ ) {
            int vidx = i + myrand( &seed, n-1-i );
            swap( victim_set, i, vidx );
          }
        } else if( vset_target_size < vset_current_size ) {
          for( i = vset_current_size; i >= vset_target_size; i-- ) {
            int vidx = myrand( &seed, i );
            swap( victim_set, i, vidx );
          }
        }

        vset_current_size = min(vset_target_size, n-2);


        // Loop over prefetch segments
        do { 
          segment_end = min( segment_start + segment_size, vset_current_size );
#if 1
          // Loop over workers to prefetch
          for( i = segment_start; i < segment_end; i++ ) { 
            __builtin_prefetch( &(workers[victim_set[i]]->dq_bot) );
          }

          // Loop over tasks to prefetch
          for( i = segment_start; i < segment_end; i++ ) { 
            __builtin_prefetch( workers[victim_set[i]]->dq_bot );
          }
#endif
          // Loop over steal attempts
          for( i = segment_start; i < segment_end; i++ ) { 

            PR_INC( self, CTR_steal_tries );

            steal_outcome = steal( self_idx, victim_set[i], self->dq_base, 0 );

            if( steal_outcome == SO_THIEF ) thieves_found++;
            if( steal_outcome == SO_STOLE ) break;

          }
          segment_start += segment_size;
        } while( steal_outcome != SO_STOLE && segment_start < vset_current_size );

        // Compute new target size of victim set
        thief_est_x10 = 10 + ( 10 * thieves_found * (n-1) ) / vset_current_size;
        vset_target_size = ( n * thieves_per_victim_x10 ) / thief_est_x10;
        vset_target_size = min( vset_target_size, n-1 );

        // See if we should refresh victim set
        vset_refresh_timer -= vset_current_size;

      } while( steal_outcome != SO_STOLE && vset_refresh_timer > 0 && more_work );

      if( yield_interval != 0 ) {
        // sched_yield( );
      }

    } while( steal_outcome != SO_STOLE && more_work );

    if( steal_outcome == SO_STOLE ) {
      PR_INC( self, CTR_steals );
    }

  } while( more_work );

  return NULL;
}

#else

static void *do_work( void *arg )
{
  Worker **self_p = (Worker **) arg;
  Worker *self;
  unsigned int self_idx = self_p - workers;
  unsigned int victim_idx = self_idx;
  unsigned int seed = self_idx;
  unsigned int n = n_workers;
  int more, i=0;
  int local_sleep_interval = sleep_interval;
  int attempts = 0, next_yield = yield_interval, next_sleep = local_sleep_interval;
  int is_old_thief = 0;
  int next_spin = n-1;
  volatile int v;

  init_worker( self_idx );
  wait_for_init_done();

  self = workers[self_idx];

  #if LOG_EVENTS
    sync_clocks( self );
  #endif


  do {
    int steal_outcome;

    // Computing a random number for every steal is too slow, so we do some amount of
    // sequential scanning of the workers and only randomize once in a while, just 
    // to be sure.

    if( attempts > next_spin ) {
      if( attempts > next_yield ) {
        if( attempts > next_sleep ) {
          if( !is_old_thief ) {
            is_old_thief = new_old_thief();
          }
          next_sleep += local_sleep_interval;
        } else {
          sched_yield( );
        }
        next_yield += yield_interval;
      } else {
        v = spin( self, backoff_mode );
        next_spin += n-1;
      }
    }

    if( i>0 ) {
      i--;
      victim_idx ++;
      // A couple of if's is faster than a %...
      if( victim_idx == self_idx ) victim_idx++;
      if( victim_idx >= n ) victim_idx = 0; 
    } else {
      if( rand_interval > 0 ) {
        i = myrand( &seed, rand_interval );
      } else {
        i = 0;
      }
      victim_idx = ( myrand( &seed, n-1 ) + self_idx + 1 ) % n;
    }

    PR_INC( self, CTR_steal_tries );

    steal_outcome = steal( self_idx, victim_idx, self->dq_base, is_old_thief );
    attempts++;
    if( steal_outcome != SO_BUSY ) {
       PR_INC( self, CTR_steal_locks );
    }
    if( steal_outcome == SO_STOLE ) {
      PR_INC( self, CTR_steals );

      // Ok, this is clunky, but the idea is to record if we had to try many times
      // before we managed to steal.

      if( attempts == 1 ) {
        PR_INC( self, CTR_steal_1s );
        PR_ADD( self, CTR_steal_1t, attempts );
      } else if( attempts < n ) {
        PR_INC( self, CTR_steal_ps );
        PR_ADD( self, CTR_steal_pt, attempts );
      } else if( attempts < 3*n ) {
        PR_INC( self, CTR_steal_hs );
        PR_ADD( self, CTR_steal_ht, attempts );
      } else {
        PR_INC( self, CTR_steal_ms );
        PR_ADD( self, CTR_steal_mt, attempts );
      }
      attempts = 0;
      next_yield = yield_interval;
      next_sleep = sleep_interval;
      next_spin = n-1;
      local_sleep_interval = sleep_interval;
      is_old_thief = 0;
    }

#if SYNC_MORE
    wool_lock( &more_lock );
      more = more_work;
    wool_unlock( &more_lock );
#else
    more = more_work;
#endif
  } while( more );

  return NULL;
}

#endif

static Task *start_workers( void )
{
  int i, n = n_workers;

  if( sizeof( Worker ) % LINE_SIZE != 0 || sizeof( Task ) % LINE_SIZE != 0 ) {
    fprintf( stderr, "Unaligned Task (%lu) or Worker (%lu) size\n",
                     sizeof(Task), sizeof(Worker) );
    exit(1);
  }

  if( n_stealable == 0 ) {
    n_stealable = 3;
    for( i=n_workers; i>0; i >>= 1 ) {
      n_stealable += 2;
    }
    n_stealable -= n_stealable/4;
    if( n_workers == 1 ) {
      n_stealable = 0;
    }
  }

  if( max_old_thieves == -1 ) {
    max_old_thieves = n_workers / 4 + 1;
  }

  workers = (Worker **) memalign( sizeof(Worker*), n * sizeof(Worker) );
  ts      = (pthread_t *) malloc( (n-1) * sizeof(pthread_t) );

  pthread_attr_init( &worker_attr );
  pthread_attr_setscope( &worker_attr, PTHREAD_SCOPE_SYSTEM );

  for( i=0; i < n-1; i++ ) {
    pthread_create( ts+i, &worker_attr, &do_work, workers+i+1 );
  }

  init_worker( 0 );
  wait_for_init_done( );

  #if LOG_EVENTS
    first_time = gethrtime( );
  #endif

  #if LOG_EVENTS
    master_sync( );
    logEvent( workers[0], 1 );
  #endif

  #if WOOL_MEASURE_SPAN
    first_time = gethrtime();
    last_time = first_time;
    last_span = 0;
  #endif

  return workers[0]->dq_base;

}

#if COUNT_EVENTS

char *ctr_h[] = {
  "    Spawns",
  "   Inlined",
  "   Read",
  "   Wait",
  NULL, // "Sync lck",
  "St tries",
  NULL, // "St locks",
  " Steals",
  " L tries",
  NULL, // " L locks",
  "  Leaps",
  "     Spins",
  NULL, // "1 steal",
  NULL, // " 1 tries",
  NULL, // "p steal",
  NULL, // " p tries",
  NULL, // "h steal",
  NULL, // " h tries",
  NULL, // "m steal",
  NULL, // " m tries",
  "Sync ND",
  "Steal NI",
  "Skip try",
  "    Skip"
};

unsigned long ctr_all[ CTR_MAX ];

#endif

static void stop_workers( void )
{
  int i;
#if COUNT_EVENTS
  int j;
#endif

  #if LOG_EVENTS
    logEvent( workers[0], 2 );
  #endif

  #if WOOL_MEASURE_SPAN
    __wool_update_time();
    last_time -= first_time;
  #endif

#if SYNC_MORE
  wool_lock( &more_lock );
    more_work = 0;
  wool_unlock( &more_lock );
#else
  more_work = 0;
#endif
  wool_lock( &sleep_lock );
    wool_broadcast( &sleep_cond );
  wool_unlock( &sleep_lock );
  for( i = 0; i < n_workers-1; i++ ) {
    pthread_join( ts[i], NULL );
  }

#if WOOL_MEASURE_SPAN
  fprintf( stderr, "TIME        %10.2f ms\n", ( (double) last_time ) / 1e6 );
  fprintf( stderr, "SPAN        %10.2f ms\n", ( (double) last_span ) / 1e6 );
  fprintf( stderr, "PARALLELISM %9.1f\n\n", 
                    ( (double) last_time ) / (double) last_span );

  for( i=2; i<=8; i++ ) {
    double time_by_i = (double) last_time / i;
    double span = (double) last_span;
    double opt = span > time_by_i ? span : time_by_i;

    fprintf( stderr, "SPEEDUP %3.1f -- %3.1f on %d processors\n", 
                     (double) last_time / (span+time_by_i),
                     (double) last_time / opt,
                     i );
  }
  fprintf( stderr, "\n" );

#endif

#if COUNT_EVENTS

/* Modificado por José Fuentes: Antes del cambio */
//  fprintf( stderr, "SIZES  Worker %d  Task %d Lock %d\n", sizeof(Worker), sizeof(Task),
//                   sizeof(wool_lock_t) );

/* Modificado por José Fuentes: Despues del cambio */
  fprintf( stderr, "SIZES  Worker %lu  Task %lu Lock %lu\n", sizeof(Worker), sizeof(Task),
                   sizeof(wool_lock_t) );

  fprintf( stderr, " Worker" );
  for( j = 0; j < CTR_MAX; j++ ) {
    if( ctr_h[j] != NULL ) {
      fprintf( stderr, "%s ", ctr_h[j] );
      ctr_all[j] = 0;
    }
  }
  for( i = 0; i < n_workers; i++ ) {
    unsigned *lctr = workers[i]->ctr;
    lctr[ CTR_spawn ] = lctr[ CTR_inlined ] + lctr[ CTR_read ] + lctr[ CTR_waits ];
    fprintf( stderr, "\nSTAT %2d", i );
    for( j = 0; j < CTR_MAX; j++ ) {
      if( ctr_h[j] != NULL ) {
        ctr_all[j] += lctr[j];
        fprintf( stderr, "%*u ", (int) strlen( ctr_h[j] ), lctr[j] );
      }
    }
  }
  fprintf( stderr, "\n    ALL" );
  for( j = 0; j < CTR_MAX; j++ ) {
    if( ctr_h[j] != NULL ) {
      fprintf( stderr, "%*lu ", (int) strlen( ctr_h[j] ), ctr_all[j] );
    }
  }
  fprintf( stderr, "\n" );

#endif

#if LOG_EVENTS

  fprintf( stderr, "\n" );
  for( i = 1; i < n_workers; i++ ) {
    fprintf( stderr, "CLOCK %d, %15lld %15lld\n", i, diff[i], trip[i] );
  }
  fprintf( stderr, "\n" );

  for( i = 0; i < n_workers; i++ ) {
    LogEntry *p;

    fprintf( stderr, "\n" );
    for( p = logbuff[i]; p < workers[i]->logptr; p++ ) {
      fprintf( stderr, "EVENT %2d %3d %15lld\n", i, p->what, p->time+diff[i]-first_time );
    }
  }

#endif

#if 1
  for( i = 0; i < n_workers; i++ ) {
    Worker *w = workers[i];
    fprintf( stderr, "", w->dq_bot - w->dq_base );
    // fprintf( stderr, "%ld\n", w->dq_bot - w->dq_base );
  }
#endif

}


int main( int argc, char **argv )
{
  Task  *__dq_top;
  int result,i;

  n_workers = 1;

  opterr = 0;

  // An old Solaris box I love does not support long options...
  while( 1 ) {
    int c;

    c = getopt( argc, argv, "p:s:t:y:d:i:o:b:r:e:c:f:g:x:" );

    if( c == -1 || c == '?' ) break;

    switch( c ) {
      case 'p': n_workers = atoi( optarg );
                break;
      case 's': n_stealable = atoi( optarg );
                break;
#if 1
      case 'b': backoff_mode = atoi( optarg );
                break;
      case 'r': rand_interval = atoi( optarg );
                break;
#endif
      case 't': init_worker_dq_size = atoi( optarg );
                break;
      case 'y': yield_interval = atoi( optarg );
                break;
      case 'i': sleep_interval = atoi( optarg );
                break;
      case 'o': max_old_thieves = atoi( optarg );
                break;
      case 'e': event_mask = atoi( optarg );
                break;
#if WOOL_MEASURE_SPAN
      case 'c': __wool_sc = (hrtime_t) atoi( optarg );
                break;
#endif
#if WOOL_STEAL_SET
      case 'f': global_refresh_interval  = atoi( optarg );
                break;
      case 'g': global_segment_size  = atoi( optarg );
                break;
      case 'x': global_thieves_per_victim_x10  = atoi( optarg );
                break;
#endif
    }
  }

  for( i = 1; i < argc-optind+1; i++ ) {
    argv[i] = argv[ i+optind-1 ];
  }

  __dq_top = start_workers( );

  result = CALL( main, argc-optind+1, argv );

  stop_workers( );

  return result;

}
