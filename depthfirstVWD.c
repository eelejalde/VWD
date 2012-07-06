/*****************************************************************************
 * DEPTH FIRST VISHKIN CODE                                                   *
 *****************************************************************************/

#include <sys/time.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <malloc.h>
#include "types.h"
#include "wool.h"

int numThreads = 2;
int n_workers;
int *totals;

/* auxiliar methods */

int get_data(const char* s, TDATA d[]) {
  FILE *fp = fopen(s, "r");

  if (!fp) {
    printf("Error opening file \"%s\".\n", s);
    exit(-2); /*Couldn't find file.*/
  }

  printf("Reading data from file: %s\n", s);
  printf ("Maximum text size is set to %u\n", BIGSTRINGLEN);

	/*Read the text-pattern file, except lines marked with "%"*/
	/*Files can be at most 64 bytes long, patterns can be at most 255
	 bytes long*/
	char line[LINE_WIDTH];
	int n = 0;

	while (fgets(line, sizeof(line), fp) != 0) {

		if (n == NUM_TEST_FILES) {
			printf("%d lines have been read. I'm skipping the rest.\n",
					NUM_TEST_FILES);
			break;
		}

		if (*line == '%') {
			continue;
		}

		char *spc = strchr(line, ' ');
		memcpy(d[n].path, line, spc - line);
		d[n].path[spc - line] = '\0';

		spc++;

		char *end = strchr(spc, '\n');
		if (end) {
			memcpy(d[n].pattern, spc, end - spc);
			d[n].pattern[end - spc] = '\0';
		} else
			strcpy(d[n].pattern, spc);

		n++;
	}

	printf("%d text file(s) to be processed.", n);
	fclose(fp);

	return n;
}

int read_text_from_file(const char* fn, char* t) {

	FILE* fp = fopen(fn, "r");
	if (!fp) {
		printf("Error opening file \"%s\".\n", fn);
		return -3;
	}
	if(fread(t, BIGSTRINGLEN, 1, fp)){};
	fclose(fp);
	return 0;
}


/* Main methods */

static int isRealMatch(int c, int m, const char *t, const char *p) {
        int i;
        if (c != -1) {
                for (i = 0; i < m; i++) {
                        if (t[c + i] != p[i]) {
                                return 0;
                        }
                }
//                printf("%d\n", i);
                //printf("Match found at \"%i\".\n", c);
                return 1;
        }
        return 0;
}

static int duelText(int j1, int j2, const char *t, const char *p, int *wit) {
        __builtin_prefetch(t + j1);
        if (j1 == -1) {
                return j2;
        }
        if (j2 == -1) {
                return j1;
        }

        int *w = wit + j1 - j2;
        int mj2 = t[j1 + *w] == p[j1 - j2 + *w];
        int mj1 = t[j1 + *w] == p[*w];

//	printf("d\n");

        if (!mj1 && !mj2)
                return -1;
        if (mj1 > mj2)
                return j1;
        else
                return j2;
}

LOOP_BODY_1(eliminateCandidates, LARGE_BODY, int, active, void *, arg)
{
  struct partition_args *pargs = 
    (struct partition_args *) arg;
  
  pargs->activePartition = active;

  int *readCandidates, *writeCandidates, 
    *witness, *tmp;
  int numPartitions, activePartition, 
    totalNumTrees, from, to, maxK,
    treeBlockSize, matches, i, j, k, 
    threadNumTrees;

  matches = 0;

  /*unpack struct*/
  const char *p = pargs->pattern;
  const char *t = pargs->buffer;
  size_t m = pargs->pattern_len;
  numPartitions = pargs->numPartitions;
  totalNumTrees = pargs->numBlocks;
  maxK = pargs->maxK;
  activePartition = pargs->activePartition;
  witness = pargs->witness;
  treeBlockSize = pargs->treeBlockSize;

  /* Validations */
  if (totalNumTrees / numPartitions < 1) {
    return;
  }
  if (activePartition > numPartitions - 1) {
    return;
  }

  threadNumTrees = 
    (int) ceil(totalNumTrees / (double) numPartitions);
  from = threadNumTrees * treeBlockSize * activePartition;
  to = 
    treeBlockSize * threadNumTrees * (activePartition + 1) - 1;

  // If this is the last partition,
  // then only the remaining blocks 
  //will be assigned to this thread
  if (activePartition == numPartitions - 1) {
    to = totalNumTrees * treeBlockSize;
    threadNumTrees = (to - from) / treeBlockSize;
  }

  if(posix_memalign((void**)&readCandidates, 
		    64, treeBlockSize * sizeof(int)))
    {
      printf("Error: Out of memory in readCandidates\n");
      exit(EXIT_FAILURE);
    }
  if(posix_memalign((void**)&writeCandidates, 
		    64, treeBlockSize * sizeof(int)))
    {
      printf("Error: Out of memory in writeCandidates\n");
      exit(EXIT_FAILURE);
    }

  //        readCandidates = (int *) memalign(64, treeBlockSize * sizeof(int));
  //        writeCandidates = (int *) memalign(64, treeBlockSize / 2 * sizeof(int));
  // Let the duels begin
  // If maxK is 1, then no duels will be performed
  if (maxK == 1)
    for (i = from; i < to; i++) 
      matches += isRealMatch(i, m, t, p);
  else {
    for (i = 0; i < threadNumTrees; i++) {
      for (j = 0; j < treeBlockSize; j++) {
	readCandidates[j] = 
	  (int)(from + (i * treeBlockSize) + j);
      }

      // Let's build the tree now
      for (k = 1; k < maxK; k++) {
	for (j = 0; j < treeBlockSize; j += 2) {
	  writeCandidates[j / 2] = 
	    duelText(readCandidates[j + 1], readCandidates[j],
		     t, p, witness);
	}

	// Switching read and write candidates
	tmp = writeCandidates;
	writeCandidates = readCandidates;
	readCandidates = tmp;
	
	// Moving to the next tree level
	treeBlockSize /= 2;
      }

      // Finding real matches
      for (j = 0; j < treeBlockSize; j++) {
	matches += 
	  isRealMatch(readCandidates[j], m, t, p);
      }
      
      // Resetting treeBlockSize
      treeBlockSize = pargs->treeBlockSize;
    }
  }
  
  free(readCandidates);
  free(writeCandidates);
  //printf("Matches found: %i\n", matches);
  totals[active] = matches;
}

TASK_4(void*, df_vishkin_search, const char*, P, const char*, T, size_t, m, size_t, n) {

        int pos, i, maxK, treeBlockSize, numTrees, witnessSize;
        int *witness;
        size_t num_matches = 0;

        // Preprocessing
        witnessSize = ((m + 1) / 2) + 1;
        witness = (int *) malloc(witnessSize * sizeof(int));
        *witness = -1;

        for (pos = 1; pos < witnessSize; pos++) {
                witness[pos] = -1;

                for (i = 0; i < m - pos; i++) {
                        if (P[i] != P[i + pos]) {
                                witness[pos] = i;
                                break;
                        }
                }

                if (witness[pos] == -1) {
                        printf("There's a period in the pattern. Exiting.\n");
                }
        }

        maxK = (int) log2(m);
        treeBlockSize = (int) pow(2, maxK);
        numTrees = (n - m) / treeBlockSize;

        struct partition_args pa;
        pa.numBlocks = numTrees;
        pa.numPartitions = numThreads;
        pa.maxK = maxK;
        pa.pattern = P;
        pa.pattern_len = m;
        pa.buffer = T;
        pa.buffer_len = n;
        pa.treeBlockSize = treeBlockSize;
        pa.witness = witness;


		/* Searching */
      FOR(eliminateCandidates, 0, numThreads, (void *) &pa);

		for ( i = numTrees * treeBlockSize; i < n - m + 1; i++)
			num_matches += isRealMatch(i, m, T, P);

		return (size_t*) num_matches;
}

TASK_2(int, main, int, argc, char**, argv) {
  numThreads = n_workers;
  struct timeval stime, etime;
  char * bigstring;
  size_t res;
  double t;
  int i;
  
  if (argc <= 1) {
    printf("Usage: tpar <text-pattern file>.\n");
    return -1; /*Wrong number of arguments*/
  }
  
  TDATA d[NUM_TEST_FILES];
  
  /* Allocate space for long-string tests. */
  if (!(bigstring = calloc(1, BIGSTRINGLEN))) {
    fprintf(stderr, "Out of memory");
    exit(1);
  }
  
  int n = get_data(argv[1], d);
 
  /* Test each function. */
  int ii;
  for (ii = 0; ii < n; ii++) {
     
       totals = malloc(sizeof(int)*numThreads);

       if (read_text_from_file(d[ii].path, bigstring))
         continue;
    
       printf("\n%d. Processing file: %s (%lu bytes), pattern: %s\n", ii + 1, d[ii].path, strlen(bigstring), d[ii].pattern);

       if (gettimeofday(&stime, NULL)) {
         fprintf(stderr, "gettimeofday failed");
         exit(1);
       }

       res = (size_t)CALL(df_vishkin_search, d[ii].pattern, bigstring, strlen(d[ii].pattern), strlen(bigstring));

       if (gettimeofday(&etime, NULL)) {
         fprintf(stderr, "gettimeofday failed");
         exit(1);
       }


       /* Compute time taken and add it to the sums. */
       t = (etime.tv_sec - stime.tv_sec) + (etime.tv_usec - stime.tv_usec)/ 1000000.0;

      for(i=0; i< numThreads; i++)
		res += totals[i];

      printf("depth_first_vishkin: %lf ms (%zu matches)\n", t, res);
	  free(totals);
    }
	    
    memset(bigstring, '\0', BIGSTRINGLEN);
    free(bigstring);
    printf("Bye.\n");

    return 0;
}
