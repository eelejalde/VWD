/*
	============================================================================
	Name        : originalVWD.c
	Author      : Azu Parallel Algorithms and Systems Lab
	Version     : 1.0
	Copyright   : 
	Description : Original Vishkin Code
	Modified    : July 13, 2012
	============================================================================
*/


#include <sys/time.h>

#include "wool.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define MAX_K(m) (int)ceil(log2(m))

const char FALSE = 0;
const char TRUE = 1;

int numThreads = 2;

/* auxiliar methods */

/* Read input parameter file, line by line */
int get_data(const char* s, TDATA d[]) {
	FILE *fp = fopen(s, "r");

	if (!fp) {
		printf("Error opening file \"%s\".\n", s);
		exit(-2); /*Couldn't find file.*/
	}

	printf("Reading data from file: %s\n", s);
	printf ("Maximum text size is set to %u\n", BIGSTRINGLEN);

	/*Read the text-pattern file, except lines marked with "%"*/
	/*Files length and patterns length can be defined in types.h file */
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

/* Read the text to be search */
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
void box2(const char*, int , int**, int*, int);
void box3(const char*, int, int**, int*, int);
int* box1(const char*, int);


/* Check character by character the remaining candidate positions in the last level of LEFT */
void terminal_stage(const char *P, int m, int **Left, int *Witness, int k) {
  k--;
  int max_j = MAX_K(m)-2;

  int j, a;
  for (j = 1; j <= max_j; j++)
    if (Left[k][j] > 0) {
      for ( a = Left[k][j]; a < m; a++)
	if (P[a] != P[a-Left[k][j]]) {
	  Witness[Left[k][j]] = a-Left[k][j];
	  break;
	}
      if (Witness[Left[k][j]]==-1) {
	printf ("Periodicity found at P[%d]\n",Left[k][j]); /* In this stage of our investigation, we only work with non-periodic patterns */
	exit(EXIT_FAILURE);
      }
    }
}

/* Vishkin’s duel algorithm */
void box3(const char *P, int m, int **Left, int *Witness, int k) {

  int max_a = floor(m/pow(2,k));
  int a;

  for ( a = 1; a < max_a; a++) {
    if (Left[k-1][2*a+1] == -1)
      Left[k][a] = Left[k-1][2*a];
    else if (Left[k-1][2*a] == -1)
      Left[k][a] = Left[k-1][2*a+1];
    else {
      int j1 = Left[k-1][2*a+1];
      int j2 = Left[k-1][2*a];

      int w = Witness[j1-j2];
      char x = P[w];
      char y = P[j1-j2+w];
      char z = P[j1+w];

      if (z != y)
	Witness[j2] = j1-j2+w;

      if (z != x)
	Witness[j1] = w;

      if (Witness[j2] == -1)
	Left[k][a] = j2;
      else if (Witness[j1] == -1)
	Left[k][a] = j1;
      else
	Left[k][a] = -1;
    }
  }

  box2(P, m, Left, Witness, k);
}

void box2(const char *P, int m, int **Left, int *Witness, int k) {

  k++;
  
	/* Verify if this is the last level of the tournament tree */
  if (k>MAX_K(m)-2) {
      terminal_stage(P, m, Left, Witness, k);
      return;
  }

  if (Left[k-1][1]!=-1) {

    int x = Left[k-1][1];
    int max_j = pow(2,k+1)-x+1;
    int j;

	/* Verify k-Certainty */
    for ( j = 0; j < max_j ; j++) 
      if (P[j]!=P[x+j]) {
	Witness[x] = j;
	break;
      }

	/* If we can't verify k-certainty, then we check character by character to discard periodicity */
      if (Witness[x]==-1) {
	for ( j = max_j; j < m - x ; j++){
	    if (P[j]!=P[x+j]) {
		Witness[x] = j;
		break;
	    }	
	}
	/* The pattern is periodic */
	if (Witness[x]==-1){
		printf ("Periodicity found at P[%d]\n",x); /* In this stage of our investigation, we only work with non-periodic patterns */
		exit(EXIT_FAILURE);
	}
	else{
		/* Box3 satisfies k-sparsity (duel candidates) */
		box3(P, m, Left, Witness, k);	
	}
      } else {
	/* Box3 satisfies k-sparsity (duel candidates) */
	box3(P, m, Left, Witness, k);
      }
    }
  else
	/* Box3 satisfies k-sparsity (duel candidates) */
    box3(P, m, Left, Witness, k);
}

int *box1(const char *P, int m) {

  int k = 0, i;

  int **Left;
  
  if (( Left = ( int** )malloc( MAX_K(m)*sizeof( int* ))) == NULL ) {
    printf ("Out of memory.\n");
    exit(EXIT_FAILURE);
  }

  for ( i = 0; i < MAX_K(m); i++ )
    if (( Left[i] = calloc( m,sizeof(int) )) == NULL ) {
      printf ("Out of memory.\n");
      exit(EXIT_FAILURE);
    }

	/* Initializing level 0 for the bi-dimensional array Left (Preprocess stage) */
  for ( i = 0; i < m; i++)
    Left[0][i] = i;

  int *Witness;

  if (( Witness = malloc( m*sizeof(int) )) == NULL ) {
    printf ("Out of memory.\n");
    exit(EXIT_FAILURE);
  }

	/* Initializing witness array */
  for( i = 0; i < m; i++)
    Witness[i] = -1;

  box2(P, m, Left, Witness, k);

  for ( i = 0; i < MAX_K(m); i++) {
    int* currentIntPtr = Left[i];
    free(currentIntPtr);
  }

  free(Left);

  return Witness;
}

/* StepOne */
TASK_2(int*, step1, const char*, P, int, m) {

  return box1(P,m);

}

/* Parallelize duels in the text */
LOOP_BODY_6( mm, LARGE_BODY, int, a, int*, Witness, int**, Left, int, k, char*, Match, const char*, T, const char*, P)
{
      if (Left[k-1][2*a+1] == -1)
	Left[k][a] = Left[k-1][2*a];

      else if (Left[k-1][2*a] == -1)
	Left[k][a] = Left[k-1][2*a+1];

      else {
	int j1 = Left[k-1][2*a+1];
	int j2 = Left[k-1][2*a];
	
	int w = Witness[j1-j2];
	char x = P[w];
	char y = P[j1-j2+w];
	char z = T[j1+w];
	
	if (z != y)
	  Match[j2] = FALSE;

	if (z != x)
	  Match[j1] = FALSE;

	if (Match[j2])
	  Left[k][a] = j2;
	else if (Match[j1])
	  Left[k][a] = j1;
	else
	  Left[k][a] = -1;
	}

}

/* The search step, whereby the pattern P is searched in the text T */
TASK_6(void*, step2, const char*, T, const char*, P, size_t, n, size_t, m, int*, Witness, int**, Left) {

	/* Match array M , store potential matches of P in T */
  char *Match;
  int i, k;

  if ( (Match = calloc(sizeof(char),n-m+1)) == NULL ) {
      printf ("Out of memory.\n");
      exit(EXIT_FAILURE);
  }

	/* Initialize the Match array */
  for (i = 0; i < (n-m+1); i++) 
    Match[i] = TRUE;

  for (k = 1; k <= MAX_K(m)-1; k++) {

    int max_a = floor((n-m+1)/pow(2,k));

	/* Search using Wool parallel_for, parallelizing duels */
    FOR(mm, 0, max_a, Witness, Left, k, Match, T, P);    
  }

  return Match;

}

LOOP_BODY_6(mm_step3, LARGE_BODY, int, j, const char*, T, const char*, P, int, m, int**, Left, char*, Match, int, max_k)
{
    int t = Left[max_k][j];
    int i;
    if (t != -1)
	/* Check character by character if the candidate "t" is a real match */
      for (i = 0; i < m; i++)
  		if (T[t+i] != P[i]) {
		  Match[t] = FALSE;
		  break;
		}
}

/* "Verify" step that checks character by character for some occurrence of pattern P using candidates in the last level of LEFT array */
VOID_TASK_6(step3, const char*, T, const char*, P, int, n, int, m, int**, Left, char*, Match) {

  int max_j = (int)floor((n-m+1)/pow(2,MAX_K(m)-1));
  int max_k = MAX_K(m)-1;

	/* Parallelizing  the verification for some ocurrence of pattern P in the text T*/
  FOR(mm_step3, 0, max_j, T, P, m, Left, Match, max_k);
}

/* originalVWD search algorithm */
TASK_4(void*, original_vishkin_search, const char*, P, const char*, T, size_t, m, size_t, n) {
  
	int i;
  
        /* Preprocessing (VStepOne) */
	int *Witness = CALL(step1,P,m);

	int **Left;

	if (( Left = ( int** )malloc( (MAX_K(m))*sizeof( int* ))) == NULL ) {
	  printf ("Out of memory.\n");
	  exit(EXIT_FAILURE);
	}

	for (i = 0; i <= MAX_K(m)-1; i++ )
	  if (( Left[i] = calloc( n, sizeof(int) )) == NULL ) {
	    printf ("Out of memory.\n");
	    exit(EXIT_FAILURE);
	  }

	/* Initializing level 0 of the bi-dimensional array Left (Search stage) */
	for (i = 1; i < n; i++)
	  Left[0][i] = i;

	/* Searching using Wool parallel_for (VStepTwo) */
	char *Match = CALL(step2, T, P, n, m, Witness, Left);

	free(Witness);

	/* VStepThree */
	CALL(step3, T, P, n, m, Left, Match);

	size_t cnt = 0;
	for (i = 0; i < n-m+1; i++)
	  if (Match[i]==TRUE) {
	    cnt++;
	  }
	
	free(Match);

	for (i = 0; i < MAX_K(m)-1; i++)  
	  free(Left[i]);  

	free(Left);  
	
	return (size_t*)cnt;
}



/* Main method defined according to Wool specifications */
TASK_2(int, main, int, argc, char**, argv) {

  struct timeval stime, etime;
  char * bigstring;
  size_t res;
  double t;
  
  if (argc <= 1) {
    printf("Usage: originalVWD <text-pattern file>.\n");
    return -1; /*Wrong number of arguments*/
  }
  
  TDATA d[NUM_TEST_FILES];
  
  /* Allocate space for long-string tests. */
  if (!(bigstring = calloc(1, BIGSTRINGLEN))) {
    fprintf(stderr, "Out of memory");
    exit(1);
  }
  
  int n = get_data(argv[1], d);

  int ii;
  for (ii = 0; ii < n; ii++) {
     
       if (read_text_from_file(d[ii].path, bigstring))
         continue;
    
       printf("\n%d. Processing file: %s (%lu bytes), pattern: %s\n", ii + 1, d[ii].path, strlen(bigstring), d[ii].pattern);

       if (gettimeofday(&stime, NULL)) {
         fprintf(stderr, "gettimeofday failed");
         exit(1);
       }

	/* Call to originalVWD algorithm using Wool syntax */
       res = (size_t)CALL(original_vishkin_search, d[ii].pattern, bigstring, strlen(d[ii].pattern), strlen(bigstring));

       if (gettimeofday(&etime, NULL)) {
         fprintf(stderr, "gettimeofday failed");
         exit(1);
       }


       /* Compute time taken and add it to the sums. */
       t = (etime.tv_sec - stime.tv_sec) + (etime.tv_usec - stime.tv_usec)/ 1000000.0;

       printf("OriginalVWD: %lf ms (%zu matches)\n", t, res);
    }
	    
    memset(bigstring, '\0', BIGSTRINGLEN);
    free(bigstring);
    printf("Bye.\n");

    return 0;
}
