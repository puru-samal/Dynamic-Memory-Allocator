/*************************************************************************
 * @file mm.c                                                            *
 * @brief A 64-bit struct-based implicit free list memory allocator      *
 *                                                                       *
 * 15-213: Introduction to Computer Systems                              *
 *                                                                       *
 *                                                                       *
 * The dynamic memory allocator manages the 'heap' which is a collection *
 * of various-sized blocks. All blocks are 16-byte aligned.The blocks    *
 * are broadly divided into two categories:                              *
 *      - Allocated blocks are marked as allocated and remain allocated  *
 *        until they are explicitly free'd. They are structured as:      *
 *        | HEADER || PAYLOAD |                                          *
 *      - Free blocks are are marked are free and remain so until        *
 *        explicitly allocated. Free blocks are further divided into two *
 *        sub-categories depending on their size:                        *
 *          - A mini free block is a minimum_block_size'd block          *
 *            (16 bytes). They are structured as:                        *
 *            | HEADER || PAYLOAD (next) |                               *
 *            Since a header is 8 bytes, and the Payload is 8 bytes,     *
 *            The payload is aliased to be a pointer (next) that points  *
 *            to the next mini free block. This is used to manage a      *
 *            singly-linked list of mini-free blocks.                    *
 *         -  A standard free block is atleast 32 bytes and is           *
 *            structured as follows:                                     *
 *            | HEADER || PAYLOAD (next)(prev) || FOOTER |               *
 *            Since, the payload is atleast 16 bytes, it is aliased to   *
 *            hold pointers to the next and previous free blocks. This   *
 *            is used to manage a doubly-linked list of free-blocks.     *
 *                                                                       *
 * Headers/footers are single word-size'd and are used to encode info    *
 * requires for performing various operations:                           *
 *  - 60 MSB's encode the size of the block.                             *
 *  - LSB encodes the current allocation status of the block.            *
 *  - 2nd LSB encodes the allocation status of the previous block.       *
 *  - 3rd LSB tells us if the previous block is a mini-block or not.     *
 *                                                                       *
 *                                                                       *
 * A broad overview of the workings of my allocator is as follows:       *
 *      - Free block organization: Free blocks are managed as segregated *
 *        free lists. The segregated list is an array of pointers to     *
 *        singly/doubly linked lists that are used to manage free blocks *
 *        belonging to different size classes. The pointer at index 0    *
 *        points to a list of blocks that are min_block_size'd (16).     *
 *        This is the only list that is singly-linked, Every other size  *
 *        class is large enough to have the payload alias'd to two       *
 *        pointers, making it possible for them to be managed as doubly  *
 *        linked lists.                                                  *
 *      - Placement: An approximation of the Best-fit policy is used.    *
 *        Segregated lists are used as the initial approximator to find  *
 *        the best fit. An attempt is made to further improve the        *
 *        best-fit approximation by searching for the next 6 blocks that *
 *        also satisfy the size criterion and choosing the smallest      *
 *        possible block to reduce fragmentation. See find_fit function  *
 *        for more information.                                          *
 *      - Splitting: If the block size is large enough to merit a split  *
 *        while maintaining alignment, an allocated block is split into  *
 *        an allocated and free block. The prev_alloc and prev_mini      *
 *        flags of the next block are set appropriately. See split_block *
 *        function for more information.                                 *
 *      - Coalescing: Adjacent free blocks are immediately merged to     *
 *        combat false fragmentation. See coalecse_block function for    *
 *        more information.                                              *
 *                                                                       *
 *************************************************************************
 *************************************************************************
 *
 * @author Puru Samal <psamal@andrew.cmu.edu>
 */

#include "mm.h"
#include "memlib.h"
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Do not change the following! */

#ifdef DRIVER
/* create aliases for driver tests */
#define malloc mm_malloc
#define free mm_free
#define realloc mm_realloc
#define calloc mm_calloc
#define memset mem_memset
#define memcpy mem_memcpy
#endif /* def DRIVER */

/* You can change anything from here onward */

/*
 *****************************************************************************
 * If DEBUG is defined (such as when running mdriver-dbg), these macros      *
 * are enabled. You can use them to print debugging output and to check      *
 * contracts only in debug mode.                                             *
 *                                                                           *
 * Only debugging macros with names beginning "dbg_" are allowed.            *
 * You may not define any other macros having arguments.                     *
 *****************************************************************************
 */
#ifdef DEBUG
/* When DEBUG is defined, these form aliases to useful functions */
#define dbg_requires(expr) assert(expr)
#define dbg_assert(expr) assert(expr)
#define dbg_ensures(expr) assert(expr)
#define dbg_printf(...) ((void)printf(__VA_ARGS__))
#define dbg_printheap(...) print_heap(__VA_ARGS__)
#else
/* When DEBUG is not defined, these should emit no code whatsoever,
 * not even from evaluation of argument expressions.  However,
 * argument expressions should still be syntax-checked and should
 * count as uses of any variables involved.  This used to use a
 * straightforward hack involving sizeof(), but that can sometimes
 * provoke warnings about misuse of sizeof().  I _hope_ that this
 * newer, less straightforward hack will be more robust.
 * Hat tip to Stack Overflow poster chqrlie (see
 * https://stackoverflow.com/questions/72647780).
 */
#define dbg_discard_expr_(...) ((void)((0) && printf(__VA_ARGS__)))
#define dbg_requires(expr) dbg_discard_expr_("%d", !(expr))
#define dbg_assert(expr) dbg_discard_expr_("%d", !(expr))
#define dbg_ensures(expr) dbg_discard_expr_("%d", !(expr))
#define dbg_printf(...) dbg_discard_expr_(__VA_ARGS__)
#define dbg_printheap(...) ((void)((0) && print_heap(__VA_ARGS__)))
#endif

/* Basic constants */

typedef uint64_t word_t;

/** @brief Word and header size (bytes) */
static const size_t wsize = sizeof(word_t);

/** @brief Double word size (bytes) | Memory must be aligned to this size */
static const size_t dsize = 2 * wsize;

/** @brief Minimum block size (bytes)
 *  Minimum possible block size is 2 * wsize. 1 word for header and atlease
 *  one word for the payload to ensure alignment. */
static const size_t min_block_size = dsize;

/** @brief amount a heap can be extended by.
 * (Must be divisible by dsize)
 */
static const size_t chunksize = (1 << 6);

/** @brief addr & alloc_mask gets the allocated flag
 */
static const word_t alloc_mask = 0x1;

/** @brief addr & prev_alloc_mask gets the allocated flag of the previous block
 */
static const word_t prev_alloc_mask = 0x1 << 1;

/** @brief addr & prev_miniblock_mask gets the miniblock status of the previous
 * block
 */
static const word_t prev_miniblock_mask = 0x1 << 2;

/** @brief The manimum number of blocks to check for to improve the best-fit
 * approximation
 */
static const size_t max_search = 6;

/** @brief addr & size_mask gets the size flag
 */
static const word_t size_mask = ~(word_t)0xF;

/** @brief Represents the header and payload of one block in the heap */
typedef struct block {
    /** @brief Header contains size + allocation flag */
    word_t header;

    /**
     * @brief A pointer to the block payload.
     */
    union {
        struct {
            struct block *next;
            struct block *prev;
        } node;
        char payload[0];
    } un;

} block_t;

/* Global variables */

/** @brief Pointer to first block in the heap */
static block_t *heap_start = NULL;

#define NUM_CLASSES 15

/** @brief Array of pointers to the head of the explicit_free_lists segregated
 * into diffrent classes */
static block_t *seg_list[NUM_CLASSES];

/*
 *****************************************************************************
 * The functions below are short wrapper functions to perform                *
 * bit manipulation, pointer arithmetic, and other helper operations.        *
 *****************************************************************************
 */

/*
 * ---------------------------------------------------------------------------
 *                        BEGIN SHORT HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Returns the maximum of two integers.
 * @param[in] x
 * @param[in] y
 * @return `x` if `x > y`, and `y` otherwise.
 */
static size_t max(size_t x, size_t y) {
    return (x > y) ? x : y;
}

/**
 * @brief Rounds `size` up to next multiple of n
 * @param[in] size
 * @param[in] n
 * @return The size after rounding up
 */
static size_t round_up(size_t size, size_t n) {
    return n * ((size + (n - 1)) / n);
}

/**
 * @brief Packs the `size` and `alloc` of a block into a word suitable for
 *        use as a packed value.
 *
 * Packed values are used for both headers and footers.
 *
 * The allocation status is packed into the lowest bit of the word.
 *
 * @param[in] size The size of the block being represented
 * @param[in] alloc True if the block is allocated
 * @return The packed value
 */
static word_t pack(size_t size, bool alloc, bool prev_alloc,
                   bool prev_miniblock) {
    word_t word = size;
    if (alloc) {
        word |= alloc_mask;
    }
    if (prev_alloc) {
        word |= prev_alloc_mask;
    }
    if (prev_miniblock) {
        word |= prev_miniblock_mask;
    }
    return word;
}

/**
 * @brief Extracts the size represented in a packed word.
 *
 * This function simply clears the lowest 4 bits of the word, as the heap
 * is 16-byte aligned.
 *
 * @param[in] word
 * @return The size of the block represented by the word
 */
static size_t extract_size(word_t word) {
    return (word & size_mask);
}

/**
 * @brief Extracts the size of a block from its header.
 * @param[in] block
 * @return The size of the block
 */
static size_t get_size(block_t *block) {
    return extract_size(block->header);
}

/**
 * @brief Given a payload pointer, returns a pointer to the corresponding
 *        block.
 * @param[in] bp A pointer to a block's payload
 * @return The corresponding block
 */
static block_t *payload_to_header(void *bp) {
    return (block_t *)((char *)bp - offsetof(block_t, un));
}

/**
 * @brief Given a block pointer, returns a pointer to the corresponding
 *        payload.
 * @param[in] block
 * @return A pointer to the block's payload
 * @pre The block must be a valid block, not a boundary tag.
 */
static void *header_to_payload(block_t *block) {
    dbg_requires(get_size(block) != 0);
    return (void *)(block->un.payload);
}

/**
 * @brief Given a block pointer, returns a pointer to the corresponding
 *        footer.
 * @param[in] block
 * @return A pointer to the block's footer
 * @pre The block must be a valid block, not a boundary tag.
 */
static word_t *header_to_footer(block_t *block) {
    dbg_requires(get_size(block) != 0 &&
                 "Called header_to_footer on the epilogue block");
    return (word_t *)(block->un.payload + get_size(block) - dsize);
}

/**
 * @brief Given a block footer, returns a pointer to the corresponding
 *        header.
 *
 * The header is found by subtracting the block size from
 * the footer and adding back wsize.
 *
 * If the prologue is given, then the footer is return as the block.
 *
 * @param[in] footer A pointer to the block's footer
 * @return A pointer to the start of the block
 */
static block_t *footer_to_header(word_t *footer) {
    size_t size = extract_size(*footer);
    if (size == 0) {
        return (block_t *)footer;
    }
    return (block_t *)((char *)footer + wsize - size);
}

/**
 * @brief Returns the payload size of a given block.
 *
 * The payload size is equal to the entire block size minus the sizes of the
 * block's header and footer.
 *
 * @param[in] block
 * @return The size of the block's payload
 */
static size_t get_payload_size(block_t *block) {
    size_t asize = get_size(block);
    return asize - wsize;
}

/**
 * @brief Returns the allocation status of a given header value.
 *
 * This is based on the lowest bit of the header value.
 *
 * @param[in] word
 * @return The allocation status correpsonding to the word
 */
static bool extract_alloc(word_t word) {
    return (bool)(word & alloc_mask);
}

/**
 * @brief Returns the allocation status of a block, based on its header.
 * @param[in] block
 * @return The allocation status of the block
 */
static bool get_alloc(block_t *block) {
    return extract_alloc(block->header);
}

/**
 * @brief Returns the previous blocks allocation status of a given the current
 * block's header value.
 *
 * This is based on the 2nd lowest bit of the header value.
 *
 * @param[in] word
 * @return The previous blocks allocation status correpsonding to the word
 */
static bool extract_prev_alloc(word_t word) {
    return (bool)(word & prev_alloc_mask);
}

/**
 * @brief Returns the allocation status of the previous block, based on the
 * current blocks header.
 * @param[in] block
 * @return The allocation status of the previous block
 */
static bool get_prev_alloc(block_t *block) {
    return extract_prev_alloc(block->header);
}

/**
 * @brief Returns the miniblock status of the previous block given the current
 * block's header value.
 *
 * This is based on the lowest bit of the header value.
 *
 * @param[in] word
 * @return The previous block's miniblock status correpsonding to the word
 */
static bool extract_prev_mini(word_t word) {
    return (bool)(word & prev_miniblock_mask);
}

/**
 * @brief Returns the miniblock status of the previous block, based on the
 * current block's header.
 * @param[in] block
 * @return The miniblock status of the previous block
 */
static bool get_prev_mini(block_t *block) {
    return extract_prev_mini(block->header);
}

/**
 * @brief Writes an epilogue header at the given address.
 *
 * The epilogue header has size 0, and is marked as allocated.
 *
 * @param[out] block The location to write the epilogue header
 */
static void write_epilogue(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires((char *)block == (char *)mem_heap_hi() - 7);
    block->header = pack(0, true, false, false);
}

/**
 * @brief Writes a block starting at the given address.
 *
 * This function writes both a header and optionally footer, where the location
 * of the footer is computed in relation to the header. Only free blocks require
 * a footer.
 *
 * @precontitions:
 * block != Null, must be properly aligned and within head bounds
 * size request must be non-negative and must be appropriate to maintain
 * alignment.
 *
 * @param[out] block The location to begin writing the block header
 * @param[in] size The size of the new block
 * @param[in] alloc The allocation status of the new block
 */
static void write_block(block_t *block, size_t size, bool alloc,
                        bool prev_alloc, bool prev_mini) {
    dbg_requires(block != NULL && (size_t)block % dsize);
    dbg_requires((size_t)block > (size_t)mem_heap_lo() &&
                 (size_t)block < (size_t)mem_heap_hi());
    dbg_requires(size >= 0);
    dbg_requires(size % dsize == 0);

    block->header = pack(size, alloc, prev_alloc, prev_mini);

    if (!alloc && size != 0) {
        word_t *footerp = header_to_footer(block);
        *footerp = pack(size, alloc, prev_alloc, prev_mini);
    }
}

/**
 * @brief Finds the next consecutive block on the heap.
 *
 * This function accesses the next block in the "implicit list" of the heap
 * by adding the size of the block.
 *
 * @param[in] block A block in the heap
 * @return The next consecutive block on the heap
 * @pre The block is not the epilogue
 */
static block_t *find_next(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(get_size(block) != 0 &&
                 "Called find_next on the last block in the heap");
    return (block_t *)((char *)block + get_size(block));
}

/**
 * @brief Finds the footer of the previous block on the heap.
 * @param[in] block A block in the heap
 * @return The location of the previous block's footer
 */
static word_t *find_prev_footer(block_t *block) {
    // Compute previous footer position as one word before the header
    return &(block->header) - 1;
}

/**
 * @brief Finds the previous consecutive block on the heap.
 *
 * This is the previous block in the "implicit list" of the heap.
 *
 * The position of the previous block is found by reading the previous
 * block's footer to determine its size, then calculating the start of the
 * previous block based on its size.
 * If the previous block is a miniblock, it has no footer. So, the previous
 * block is found by simply offsetting by min_block_size from the header.
 *
 * @param[in] block A block in the heap
 * @return The previous consecutive block in the heap.
 * @pre The block is not the prologue
 */
static block_t *find_prev(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(get_size(block) != 0 &&
                 "Called find_prev on the first block in the heap");

    if (get_prev_mini(block)) {
        return (block_t *)((char *)block - min_block_size);
    }
    word_t *footerp = find_prev_footer(block);
    return footer_to_header(footerp);
}

/*
 * ---------------------------------------------------------------------------
 *                        END SHORT HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/*
 *****************************************************************************
 * The functions below are short wrapper functions to perform                *
 * to manage explicit lists of free blocks.                                  *
 *****************************************************************************
 */
/*
 * ---------------------------------------------------------------------------
 *                        BEGIN EXPLICIT-FREE-LIST FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/**
 * @brief A helper function to check if b is in the list that (*head) points to
 * @param[in] head a pointer to the head of an explicit-free list
 * @param[in] b A block in the heap
 * @return True is block is in the appropriate explicit-free list else false
 */
static bool is_in(block_t **head, block_t *b) {
    for (block_t *tmp = (*head); tmp != NULL; tmp = tmp->un.node.next) {
        if (tmp == b)
            return true;
    }
    return false;
}

/**
 * @brief Adds a element to the head of the list
 * @param[in] head a pointer to the head of an explicit-free list
 * @param[in] b A block in the heap
 * @return
 */
static void insert_head(block_t **head, block_t *b) {
    dbg_requires(head != NULL);
    dbg_requires(b != NULL);
    dbg_requires(!get_alloc(b));
    dbg_requires(!is_in(head, b));

    /* miniblocks have no prev pointers. Must be handled sepatately */
    if (get_size(b) == min_block_size) {

        if ((*head) == NULL) {
            (*head) = b;
            b->un.node.next = NULL;
        } else {
            b->un.node.next = (*head);
            (*head) = b;
        }

        return;
    }

    if ((*head) == NULL) {
        (*head) = b;
        b->un.node.next = NULL;
        b->un.node.prev = NULL;
    } else {
        (*head)->un.node.prev = b;
        b->un.node.next = (*head);
        b->un.node.prev = NULL;
        (*head) = b;
    }

    dbg_ensures(is_in(head, b));

    return;
}

/**
 * @brief Removes an element from the head of the list
 * @param[in] head a pointer to the head of an explicit-free list
 * @return
 */
static void remove_head(block_t **head) {
    dbg_requires(head != NULL);
    dbg_requires(*head != NULL);

    block_t *b = (*head);

    /* miniblocks have no prev pointers. Must be handled sepatately */
    if (get_size(b) == min_block_size) {

        if ((*head)->un.node.next == NULL) {
            (*head) = NULL;
        } else {
            (*head) = (*head)->un.node.next;
        }
        b->un.node.next = NULL;

        dbg_ensures(!is_in(head, b));
        return;
    }

    if ((*head)->un.node.next == NULL) {
        (*head) = NULL;
    } else {
        (*head) = (*head)->un.node.next;
        (*head)->un.node.prev = NULL;
    }
    b->un.node.next = NULL;

    dbg_ensures(!is_in(head, b));
    return;
}

/**
 * @brief Splices out an element the list
 * Also, sets the prev and next pointers
 * which are used to splice in a free block
 * to the position that was previously occupied
 * by b.
 * @param[in] head
 * @param[in] b - block to splice out
 * @return
 */
static void remove_block(block_t **head, block_t *b) {
    dbg_requires(head != NULL);
    dbg_requires(*head != NULL);
    dbg_requires(b != NULL);
    dbg_requires(is_in(head, b));

    /* miniblocks have no prev pointers. Must be handled sepatately */
    if (get_size(b) == min_block_size) {

        if ((*head) == b) {
            remove_head(head);
        } else {

            block_t *pb = (*head);
            block_t *cb = (*head)->un.node.next;

            while (cb != NULL) {

                if (cb == b) {
                    pb->un.node.next = b->un.node.next;
                    b->un.node.next = NULL;
                }

                pb = cb;
                cb = cb->un.node.next;
            }
        }

        return;
    }

    if ((*head) == b) {
        remove_head(head);
    } else {

        if (b->un.node.next == NULL && b->un.node.prev == NULL)
            return;

        if (b->un.node.prev != NULL) {
            b->un.node.prev->un.node.next = b->un.node.next;
        }
        if (b->un.node.next != NULL) {
            b->un.node.next->un.node.prev = b->un.node.prev;
        }
    }

    b->un.node.prev = NULL;
    b->un.node.next = NULL;
    dbg_ensures(!is_in(head, b));
}

/**
 * @brief A helper function that returns an index to the appropriate free-list
 * based on the size of the block
 * @param[in] sz - size of the free block
 * @return index into the array of free-list pointers corresponding to the
 * appropriate size class
 */
static size_t get_class(size_t sz) {

    size_t idx;

    // Splits for minimum block size 16
    size_t class0 = min_block_size;
    size_t class1 = (size_t)32UL;
    size_t class2 = (size_t)48UL;
    size_t class3 = (size_t)64UL;
    size_t class4 = (size_t)80UL;
    size_t class5 = (size_t)112UL;
    size_t class6 = (size_t)160UL;
    size_t class7 = (size_t)208UL;
    size_t class8 = (size_t)272UL;
    size_t class9 = (size_t)480UL;
    size_t class10 = (size_t)800UL;
    size_t class11 = (size_t)1728UL;
    size_t class12 = (size_t)3232UL;
    size_t class13 = (size_t)5536UL;
    size_t class14 = (size_t)18736UL;

    if (sz >= class0 && sz < class1) {
        /* The miniblock class. It is the only list that is singly-linked. */
        idx = 0;
    } else if (sz >= class1 && sz < class2) {
        idx = 1;
    } else if (sz >= class2 && sz < class3) {
        idx = 2;
    } else if (sz >= class3 && sz < class4) {
        idx = 3;
    } else if (sz >= class4 && sz < class5) {
        idx = 4;
    } else if (sz >= class5 && sz < class6) {
        idx = 5;
    } else if (sz >= class6 && sz < class7) {
        idx = 6;
    } else if (sz >= class7 && sz < class8) {
        idx = 7;
    } else if (sz >= class8 && sz < class9) {
        idx = 8;
    } else if (sz >= class9 && sz < class10) {
        idx = 9;
    } else if (sz >= class10 && sz < class11) {
        idx = 10;
    } else if (sz >= class11 && sz < class12) {
        idx = 11;
    } else if (sz >= class12 && sz < class13) {
        idx = 12;
    } else if (sz >= class13 && sz < class14) {
        idx = 13;
    } else {
        idx = 14; // sz >= class14
    }

    return idx;

    dbg_ensures(idx >= 0 && idx < NUM_CLASSES);
}

/*
 * ---------------------------------------------------------------------------
 *                        END EXPLICIT-FREE-LIST HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/******** The remaining content below are helper and debug routines ********/

/**
 * @brief
 *
 * If there are any adjacent free blocks, this function merges them together.
 * It also appropriately manages the free blocks in their respective explicit
 * list And also sets the prev_alloc, prev_miniblock flags of the next block
 * post merge.
 * @preconditions: The block should not be NULL and must be marked as free.
 *
 *
 * @param[in] block a block in the heap
 * @return The merged block
 */
static block_t *coalesce_block(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(!get_alloc(block));

    block_t *next_block = find_next(block);
    bool prev_alloc = get_prev_alloc(block);
    bool next_alloc = get_alloc(next_block);
    size_t curr_size = get_size(block);

    if (prev_alloc && next_alloc) { /* Case 1: Prev and Next are allocated */

        /* Add the block to the correct class of explicit free list */
        size_t class = get_class(curr_size);
        insert_head(&seg_list[class], block);

    } else if (prev_alloc && !next_alloc) /* Case 2: Next is free */
    {
        /* Splice out the next block */
        size_t next_block_size = get_size(next_block);
        size_t next_block_class = get_class(next_block_size);
        remove_block(&seg_list[next_block_class], next_block);

        /* Merge the next block with the current block */
        /* Add the current block to the correct class of explicit free list */
        curr_size += next_block_size;
        size_t curr_class = get_class(curr_size);
        write_block(block, curr_size, false, true, get_prev_mini(block));
        insert_head(&seg_list[curr_class], block);

        next_block = find_next(block);

    } else if (!prev_alloc && next_alloc) /* Case 3: Prev is free */
    {
        /* Splice out the previous block */
        block_t *prev_block = find_prev(block);
        size_t prev_block_size = get_size(prev_block);
        size_t prev_block_class = get_class(prev_block_size);
        remove_block(&seg_list[prev_block_class], prev_block);

        /* Merge the previous block with the current block */
        /* Add the previous block to the correct class of explicit free list */
        curr_size += prev_block_size;
        size_t curr_class = get_class(curr_size);
        write_block(prev_block, curr_size, false, get_prev_alloc(prev_block),
                    get_prev_mini(prev_block));
        insert_head(&seg_list[curr_class], prev_block);

        block = prev_block;
        next_block = find_next(block);

    } else /* case 4: Prev and Next are free */
    {
        /* Splice out both the previous and next blocks */
        block_t *prev_block = find_prev(block);
        size_t next_block_size = get_size(next_block);
        size_t prev_block_size = get_size(prev_block);
        size_t next_block_class = get_class(next_block_size);
        size_t prev_block_class = get_class(prev_block_size);
        remove_block(&seg_list[next_block_class], next_block);
        remove_block(&seg_list[prev_block_class], prev_block);

        /* Merge the previous and next blocks with the current block */
        /* Add the previous block to the correct class of explicit free list */
        curr_size += prev_block_size + next_block_size;
        size_t curr_class = get_class(curr_size);
        write_block(prev_block, curr_size, false, get_prev_alloc(prev_block),
                    get_prev_mini(prev_block));
        insert_head(&seg_list[curr_class], prev_block);

        block = prev_block;
        next_block = find_next(block);
    }

    /* Each case appropriately finds next_block in the heap post-merge */
    /* The prev_alloc and prev_miniblock flags of the next_block are set */
    write_block(next_block, get_size(next_block), get_alloc(next_block), false,
                get_size(block) == min_block_size);
    return block;
}

/**
 * @brief
 *
 *  This function extends the heap by size rounded to the dsize to maintain
 * alignment.
 *
 *
 * @param[in] size -  size to extend head by. Is rounded to maintain alignment.
 * @return The new free block created as a result of the heap extension
 */
static block_t *extend_heap(size_t size) {
    void *bp;

    // Allocate an even number of words to maintain alignment
    size = round_up(size, dsize);
    if ((bp = mem_sbrk((intptr_t)size)) == (void *)-1) {
        return NULL;
    }

    /* bp represents the first byte of the newly allocated heap area.
     */

    // Initialize free block header/footer
    block_t *block = payload_to_header(bp);
    write_block(block, size, false, get_prev_alloc(block),
                get_prev_mini(block));

    // Create new epilogue header
    block_t *block_next = find_next(block);
    write_epilogue(block_next);

    // Coalesce in case the previous block was free
    /// This takes care of adding the free block to the
    /// appropriate explicit-free-list
    block = coalesce_block(block);

    return block;
}

/**
 * @brief
 *
 * This function splits an allocated block into two parts if it is possible to
 * do so. It also appropriately manages the free blocks in their respective
 * explicit list And also sets the prev_alloc, prev_miniblock flags of the next
 * block post split.
 *
 * @preconditions: The block should be marked as allocated asize <= the size of
 * the block.
 *
 *
 * @param[in] block a block in the heap
 * @param[in] asize size of the allocation request
 */
static void split_block(block_t *block, size_t asize) {
    dbg_requires(get_alloc(block));
    dbg_requires(asize <= get_size(block));

    /* Since the block is now allocated */
    /* It is removed from it's explicit free list */
    size_t block_size = get_size(block);
    size_t curr_class = get_class(block_size);
    remove_block(&seg_list[curr_class], block);

    if ((block_size - asize) >= min_block_size) { // Splitting critetion is met

        /* Mark the first split as allocated */
        block_t *block_next;
        write_block(block, asize, true, get_prev_alloc(block),
                    get_prev_mini(block));

        /* Mark the second split as free */
        /* And add it to the appropriate explicit free list */
        block_next = find_next(block);
        size_t split_size = block_size - asize;
        size_t split_class = get_class(split_size);
        write_block(block_next, split_size, false, true,
                    asize == min_block_size);
        insert_head(&seg_list[split_class], block_next);

        /* Set the prev_alloc and prev_miniblock flag of the next block
         * post-split */
        block_next = find_next(block_next);
        write_block(block_next, get_size(block_next), get_alloc(block_next),
                    false, split_size == min_block_size);

    } else {

        /* Splitting criterion is not met */
        /* Set the prev_alloc and prev_miniblock flag of the next block */
        block_t *block_next = find_next(block);
        write_block(block_next, get_size(block_next), get_alloc(block_next),
                    true, block_size == min_block_size);
    }
    dbg_ensures(get_alloc(block));
}

/**
 * @brief
 *
 * This function aims to approximate the best-fit strategy
 * and finds a block that best satisfies the allocation request.
 *
 * @param[in] asize - size of the allocation request
 * @return - Block in the heap that satisfies the allocation request
 */
static block_t *find_fit(size_t asize) {

    /* Get class of the block based on the size request */
    size_t curr_class = get_class(asize);
    block_t *better_fit = NULL;
    size_t count = 0;

    /* Index into that class and begin traversing the explicit free lists */
    /* Traverse the next class list if no fit is found in the current class */
    for (size_t class = curr_class; class < NUM_CLASSES && !better_fit;
         class ++) {

        for (block_t *block = seg_list[class]; block != NULL;
             block = block->un.node.next) {

            /* If a fit is found, look at the next max_search fits */
            /* to see if a better bit is possible */
            /* If there is a class change while the search is still underway */
            /* The seaarch is terminated and the best fit found in the initial
             * class is returned */

            if (asize <= get_size(block)) {

                if (better_fit == NULL) {
                    better_fit = block;
                } else {
                    if (get_size(block) < get_size(better_fit))
                        better_fit = block;
                }
                count++;
            }

            if (count >= max_search) {
                return better_fit;
            }
        }
    }

    return better_fit; /* If no fit is found, better_fit == NULL */
}

/**
 * @brief
 *
 * Helper function that checks a block for correctness.
 *
 * @param[in] block
 * @return - True if all conditions are met, else false
 */
static bool check_block(block_t *block) {

    /* Check address alignment */
    if (!(size_t)block % dsize) {
        dbg_printf("Error: Bad alignment at %p\n", (void *)block);
        return false;
    }

    /* Check within heap boundaries */
    if (!((size_t)block > (size_t)mem_heap_lo() &&
          (size_t)block < (size_t)mem_heap_hi())) {
        dbg_printf("Error: Block %p not within heap bounds\n", (void *)block);
        return false;
    }

    /* Check minimum size of block */
    if (get_size(block) < min_block_size) {
        dbg_printf("Error: Less than minimum size at %p\n", (void *)block);
        return false;
    }

    if (!get_alloc(block)) { /* If free block */

        if (get_size(block) != min_block_size) {
            /* Check if header matches footer */
            if (get_size(block) != extract_size(*header_to_footer(block))) {
                dbg_printf("Error: Footer size does not match Header %p\n",
                           (void *)block);

                dbg_printf("Footer size: %zu | Header size: %zu\n",
                           extract_size(*header_to_footer(block)),
                           get_size(block));

                return false;
            }

            if (get_alloc(block) != extract_alloc(*header_to_footer(block))) {
                dbg_printf("Error: Footer alloc does not match Header %p\n",
                           (void *)block);
                return false;
            }
        }
    }

    /* If block is free, check if the preceding and following blocks are
     * allocated */
    if (!get_alloc(block)) {
        bool prev_alloc = get_prev_alloc(block);
        bool next_alloc = get_alloc(find_next(block));

        if (!prev_alloc) {
            dbg_printf("Error: Prev block of Block %p is free'd\n",
                       (void *)block);
            return false;
        }

        if (!next_alloc) {
            dbg_printf("Error: Next block of Block %p is free'd\n",
                       (void *)block);
            return false;
        }

        if (!prev_alloc && !next_alloc) {
            dbg_printf("Error: Prev and Next blocks of Block %p is free'd\n",
                       (void *)block);
            return false;
        }
    }

    return true;
}

/**
 * @brief
 *
 * Helper function that checks a explicit list for correctness.
 *
 * @param[in] class - the class of explicit free list to traverse
 * @param[in] num_blocks - a zero-init size_t pointer. The function updates it's
 * value to the number of free blocks it traversed through
 * @param[in] size - a zero-init size_t pointer. The function updates it's
 * value to the total size of free blocks it traversed through
 * @param[in] line - Line number where the function was called
 * @return - True if all conditions are met, else false
 */
static bool check_free_list(size_t class, size_t *num_blocks,
                            size_t *total_size, int line) {

    for (block_t *s = seg_list[class]; s != NULL; s = s->un.node.next) {

        /* Check allocation */
        if (get_alloc(s)) {
            dbg_printf("Error: Bad alloc, should be free && Line: %d\n", line);
            return false;
        }

        size_t curr_size = get_size(s);
        size_t curr_class = get_class(curr_size);

        /* Check class */
        if (curr_class != class) {
            dbg_printf("Error: Block in wrong class && Line: %d\n", line);
            return false;
        }

        /* Check within heap boundaries */
        if (!(((size_t)s > (size_t)mem_heap_lo() &&
               (size_t)s < (size_t)mem_heap_hi()))) {
            dbg_printf("Error: Out of heap boundaries && Line: %d\n", line);
            return false;
        }

        /* Only for the doubly-linked lists */
        if (curr_size != min_block_size) {
            /* Check next/prev consistency */
            block_t *f = s->un.node.next;
            if (f != NULL) {
                if (s != f->un.node.prev) {
                    dbg_printf("Error: f->prev != s && Line: %d\n", line);
                    return false;
                }
            }
        }

        (*num_blocks)++;
        (*total_size) += get_size(s);
    }

    return true;
}

/**
 * @brief
 *
 * A heap consistency checker that uses the previous helper functions
 * to ensure correctness for the heap.
 *
 * @param[in] line - Line number where the function was called
 * @return - True if all conditions are met, else false
 */
bool mm_checkheap(int line) {

    // Check Prologue
    block_t *prologue = find_prev(heap_start);

    if (get_size(prologue) != 0 || !get_alloc(prologue)) {
        dbg_printf("Error: Bad Prologue && Line: %d\n", line);
        return false;
    }

    block_t *block;
    /* Zero-init counts for implicit list traversal */
    size_t num_free_blocks = 0;
    size_t free_size = 0;
    block_t *prev_block = NULL;

    // Traverse he implicit list of blocks
    for (block = heap_start; get_size(block) > 0; block = find_next(block)) {

        /* Check each block for correctness */
        if (!check_block(block))
            return false;

        if (prev_block != NULL) {

            /* Check for curr_alloc, prev_alloc consistency */
            if (get_alloc(prev_block) != get_prev_alloc(block)) {
                dbg_printf("Error: get_alloc(prev_block) != "
                           "get_prev_alloc(block) && Line: %d\n",
                           line);
                return false;
            }

            /* Check for curr_mini, prev_miniblock consistency */
            if (get_size(prev_block) == min_block_size) {
                if (!get_prev_mini(block)) {
                    dbg_printf("Error: prev_mini mismatch && Line: %d\n", line);
                    return false;
                }
            }
        }

        prev_block = block;

        /* Maintain a count of the number of free blocks seen and the total size
         */
        if (!get_alloc(block)) {
            num_free_blocks++;
            free_size += get_size(block);
        }
    }

    /* Zero-init counts for segregated list traversal */
    size_t nblocks = 0;
    size_t tsize = 0;

    /* Now traverse the segeragated free lists */
    for (size_t class = 0; class < NUM_CLASSES; class ++) {

        /* Check if eack free list is valid */
        /* Updates  the number of free blocks and the total size of free blocks
         */
        if (!check_free_list(class, &nblocks, &free_size, line))
            return false;
    }

    /// The number of free blocks and total free size during
    /// the implicit list traversal should be equal to the
    /// number of free blocks and the total free size recorded
    /// while traversing the segregated list
    if (num_free_blocks != nblocks && free_size != tsize) {
        dbg_printf("Error: Number|Size mismatch between explicit-free-lists "
                   "and heap && Line: %d\n",
                   line);
        return false;
    }

    /* Check epilogue */
    block_t *epilogue = block;
    if (get_size(epilogue) != 0 || !get_alloc(epilogue)) {
        dbg_printf("Error: Bad Epilogue && Line: %d\n", line);
        return false;
    }

    return true;
}

/**
 * @brief
 *
 * Initializes the allocator, by creating a heap of chunksize bytes
 *
 * @return True if init is successful, false otherwise
 */
bool mm_init(void) {

    // Create the initial empty heap
    word_t *start = (word_t *)(mem_sbrk(2 * wsize));

    if (start == (void *)-1) {
        return false;
    }

    start[0] = pack(0, true, true, false); // Heap prologue (block footer)
    start[1] = pack(0, true, true, false); // Heap epilogue (block header)

    // Heap starts with first "block header", currently the epilogue
    heap_start = (block_t *)&(start[1]);

    // Initialize Seg List
    for (size_t class = 0; class < NUM_CLASSES; class ++) {
        seg_list[class] = NULL;
    }

    // Extend the empty heap with a free block of chunksize bytes
    if (extend_heap(chunksize) == NULL) {
        return false;
    }

    return true;
}

/**
 * @brief
 *
 * Allocates a block of atleast size bytes on the heap.
 *
 * @param[in] size - size of the allocation request
 * @return - A pointer to the payload of the allocated block
 */
void *malloc(size_t size) {
    dbg_requires(mm_checkheap(__LINE__));

    size_t asize;      // Adjusted block size
    size_t extendsize; // Amount to extend heap if no fit is found
    block_t *block;
    void *bp = NULL;

    // Initialize heap if it isn't initialized
    if (heap_start == NULL) {
        if (!(mm_init())) {
            dbg_printf("Problem initializing heap. Likely due to sbrk");
            return NULL;
        }
    }

    // Ignore spurious request
    if (size == 0) {
        dbg_ensures(mm_checkheap(__LINE__));
        return bp;
    }

    // Adjust block size to include overhead and to meet alignment requirements
    asize = round_up(size + wsize, dsize);
    if (asize < min_block_size)
        asize = min_block_size;

    // Search the free list for a fit
    block = find_fit(asize);

    // If no fit is found, request more memory, and then and place the block
    if (block == NULL) {
        // Always request at least chunksize
        extendsize = max(asize, chunksize);
        block = extend_heap(extendsize);
        // extend_heap returns an error
        if (block == NULL) {
            return bp;
        }
    }

    // The block should be marked as free
    dbg_assert(!get_alloc(block));

    // Mark block as allocated
    size_t block_size = get_size(block);
    write_block(block, block_size, true, get_prev_alloc(block),
                get_prev_mini(block));

    // Try to split the block if too large
    split_block(block, asize);
    bp = header_to_payload(block);

    dbg_ensures(mm_checkheap(__LINE__));
    return bp;
}

/**
 * @brief
 *
 * If ptr is NULL, does nothing. Otherwise, marks the block associated with a
 * ptr to the beginning of a block payload returned by a previous call to
 * malloc, calloc, or realloc as free.
 *
 * @param[in] bp - pointer to the payload returned by a call to malloc, calloc
 * or realloc
 */
void free(void *bp) {
    dbg_requires(mm_checkheap(__LINE__));

    if (bp == NULL) {
        return;
    }

    block_t *block = payload_to_header(bp);
    size_t size = get_size(block);

    // The block should be marked as allocated
    dbg_assert(get_alloc(block));

    // Mark the block as free
    write_block(block, size, false, get_prev_alloc(block),
                get_prev_mini(block));

    // Try to coalesce the block with its neighbors
    coalesce_block(block);

    dbg_ensures(mm_checkheap(__LINE__));
}

/**
 * @brief
 *
 * Changes the size of a previously allocated block.
 * If size is nonzero and ptr is not NULL, allocates a new block with at least
 * size bytes of payload, copies as much data from ptr into the new block as
 * will fit frees ptr. If size is nonzero but ptr is
 * NULL, does the same thing as malloc(size). If size is zero, does the same
 * thing as free(ptr).
 *
 * @param[in] ptr
 * @param[in] size
 * @return - The new block (if size > 0) | NULL (size == 0)
 */
void *realloc(void *ptr, size_t size) {
    block_t *block = payload_to_header(ptr);
    size_t copysize;
    void *newptr;

    // If size == 0, then free block and return NULL
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    // If ptr is NULL, then equivalent to malloc
    if (ptr == NULL) {
        return malloc(size);
    }

    // Otherwise, proceed with reallocation
    newptr = malloc(size);

    // If malloc fails, the original block is left untouched
    if (newptr == NULL) {
        return NULL;
    }

    // Copy the old data
    copysize = get_payload_size(block); // gets size of old payload
    if (size < copysize) {
        copysize = size;
    }
    memcpy(newptr, ptr, copysize);

    // Free the old block
    free(ptr);

    return newptr;
}

/**
 * @brief
 *
 * Allocates memory for an array of elements of size bytes each, initializes the
 * memory to all bytes zero.
 *
 * @param[in] elements - number of elements
 * @param[in] size - size of each element
 * @return - Pointer to the allocated memory
 */
void *calloc(size_t elements, size_t size) {
    void *bp;
    size_t asize = elements * size;

    if (elements == 0) {
        return NULL;
    }
    if (asize / elements != size) {
        // Multiplication overflowed
        return NULL;
    }

    bp = malloc(asize);
    if (bp == NULL) {
        return NULL;
    }

    // Initialize all bits to 0
    memset(bp, 0, asize);

    return bp;
}

/*
 *****************************************************************************
 * Do not delete the following super-secret(tm) lines!                       *
 *                                                                           *
 * 53 6f 20 79 6f 75 27 72 65 20 74 72 79 69 6e 67 20 74 6f 20               *
 *                                                                           *
 * 66 69 67 75 72 65 20 6f 75 74 20 77 68 61 74 20 74 68 65 20               *
 * 68 65 78 61 64 65 63 69 6d 61 6c 20 64 69 67 69 74 73 20 64               *
 * 6f 2e 2e 2e 20 68 61 68 61 68 61 21 20 41 53 43 49 49 20 69               *
 *                                                                           *
 * 73 6e 27 74 20 74 68 65 20 72 69 67 68 74 20 65 6e 63 6f 64               *
 * 69 6e 67 21 20 4e 69 63 65 20 74 72 79 2c 20 74 68 6f 75 67               *
 * 68 21 20 2d 44 72 2e 20 45 76 69 6c 0a c5 7c fc 80 6e 57 0a               *
 *                                                                           *
 *****************************************************************************
 */
