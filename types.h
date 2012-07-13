/*
	============================================================================
	Name        : types.h
	Author      : Azu Parallel Algorithms and Systems Lab
	Version     : 1.0
	Copyright   : 
	Description : Setting variables, constants and data types definitions
	Modified    : July 13, 2012
	============================================================================
*/

#ifndef TYPES_H_
#define TYPES_H_

/*#define BM_DEBUG 1*/

#define XSIZE 50 /*Max pattern size for KMP*/
#define ALPHABET_SIZE (1 << CHAR_BIT) /*Max sigma for Boyer-Moore*/

#ifndef max
#define MAX( a, b ) ( ((a) > (b)) ? (a) : (b) )
#endif

#define REHASH(a, b, h) ((((h) - (a)*d) << 1) + (b))

/* Length of buffer to use for performance tests: 64MB. */
#define BIGSTRINGLEN 67108865 //33554432
/*For lines in the input file.*/
#define LINE_WIDTH 288
/*Only 64 possible files with patterns.*/
#define NUM_TEST_FILES 64

/*The TDATA structure*/
struct test_data {
	char path[255];
	char pattern[255];
	int numThreads;
};

typedef struct test_data TDATA;

/* structure of thread function arguments */
struct thread_args {
	const char *pattern;
	size_t pattern_len;
	const char *buffer;
	size_t buffer_len;
	int *numThreads;
};

/* List of implementations. */
typedef struct {
	char name[50];
	void *(* func)(void *);
	int simple;
} test;

struct partition_args {
	int numBlocks;
	int numPartitions;
	int *candidates;
	char *bitCandidates;
	int *witness;
	int *lastDuel;
	int activePartition;
	int maxK;
	int treeBlockSize;
	const char *pattern;
	size_t pattern_len;
	const char *buffer;
	size_t buffer_len;
};

#endif /* TYPES_H_ */
