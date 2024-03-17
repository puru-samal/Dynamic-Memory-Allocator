/**
 * @file mm.c
 * @brief A 64-bit struct-based implicit free list memory allocator
 *
 * 15-213: Introduction to Computer Systems
 *
 * TODO: insert your documentation here. :)
 *
 *************************************************************************
 *
 * ADVICE FOR STUDENTS.
 * - Step 0: Please read the writeup!
 * - Step 1: Write your heap checker.
 * - Step 2: Write contracts / debugging assert statements.
 * - Good luck, and have fun!
 *
 *************************************************************************
 *
 * @author Your Name <andrewid@andrew.cmu.edu>
 */

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memlib.h"
#include "mm.h"

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
 * | Since the header and footer occupy 1 wsize each (dsize total),
 * min_block_size must be 2 * dsize to ensure alignment */
static const size_t min_block_size = 2 * dsize;

/** @brief amount a heap can be extended by.
 * (Must be divisible by dsize)
 */
static const size_t chunksize = (1 << 12);

/** @brief addr & alloc_mask gets the allocated flag
 */
static const word_t alloc_mask = 0x1;

/** @brief addr & size_mask gets the size flag
 */
static const word_t size_mask = ~(word_t)0xF;

/** @brief Represents the header and payload of one block in the heap */
typedef struct block {
    /** @brief Header contains size + allocation flag */
    word_t header;

    /**
     * @brief A pointer to the block payload.
     *
     * TODO: feel free to delete this comment once you've read it carefully.
     * We don't know what the size of the payload will be, so we will declare
     * it as a zero-length array, which is a GNU compiler extension. This will
     * allow us to obtain a pointer to the start of the payload. (The similar
     * standard-C feature of "flexible array members" won't work here because
     * those are not allowed to be members of a union.)
     *
     * WARNING: A zero-length array must be the last element in a struct, so
     * there should not be any struct fields after it. For this lab, we will
     * allow you to include a zero-length array in a union, as long as the
     * union is the last field in its containing struct. However, this is
     * compiler-specific behavior and should be avoided in general.
     *
     * WARNING: DO NOT cast this pointer to/from other types! Instead, you
     * should use a union to alias this zero-length array with another struct,
     * in order to store additional types of data in the payload memory.
     */
    union {
        struct {
            struct block *prev;
            struct block *next;
        } node;
        char payload[0];
    } un;

    /*
     * TODO: delete or replace this comment once you've thought about it.
     * Why can't we declare the block footer here as part of the struct?
     * Why do we even have footers -- will the code work fine without them?
     * which functions actually use the data contained in footers?
     */
} block_t;

/* Global variables */

/** @brief Pointer to first block in the heap */
static block_t *heap_start = NULL;

/** @brief Pointer to the head of the explicit_free_list */
static block_t *explicit_free_list = NULL;

/*
 *****************************************************************************
 * The functions below are short wrapper functions to perform                *
 * bit manipulation, pointer arithmetic, and other helper operations.        *
 *                                                                           *
 * We've given you the function header comments for the functions below      *
 * to help you understand how this baseline code works.                      *
 *                                                                           *
 * Note that these function header comments are short since the functions    *
 * they are describing are short as well; you will need to provide           *
 * adequate details for the functions that you write yourself!               *
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
static word_t pack(size_t size, bool alloc) {
    word_t word = size;
    if (alloc) {
        word |= alloc_mask;
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
    return asize - dsize;
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
 * @brief Writes an epilogue header at the given address.
 *
 * The epilogue header has size 0, and is marked as allocated.
 *
 * @param[out] block The location to write the epilogue header
 */
static void write_epilogue(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires((char *)block == (char *)mem_heap_hi() - 7);
    block->header = pack(0, true);
}

/**
 * @brief Writes a block starting at the given address.
 *
 * This function writes both a header and footer, where the location of the
 * footer is computed in relation to the header.
 *
 * TODO: Are there any preconditions or postconditions?
 *
 * @param[out] block The location to begin writing the block header
 * @param[in] size The size of the new block
 * @param[in] alloc The allocation status of the new block
 */
static void write_block(block_t *block, size_t size, bool alloc) {
    dbg_requires(block != NULL && (size_t)block % dsize);
    dbg_requires((size_t)block > (size_t)mem_heap_lo() &&
                 (size_t)block < (size_t)mem_heap_hi());
    dbg_requires(size > 0);
    dbg_requires(size % dsize == 0);
    dbg_requires(size >= min_block_size);

    block->header = pack(size, alloc);
    word_t *footerp = header_to_footer(block);
    *footerp = pack(size, alloc);
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
 *
 * @param[in] block A block in the heap
 * @return The previous consecutive block in the heap.
 * @pre The block is not the prologue
 */
static block_t *find_prev(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(get_size(block) != 0 &&
                 "Called find_prev on the first block in the heap");
    word_t *footerp = find_prev_footer(block);
    return footer_to_header(footerp);
}

/*
 * ---------------------------------------------------------------------------
 *                        END SHORT HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/******** The remaining content below are helper and debug routines ********/

/*
 * ---------------------------------------------------------------------------
 *                        BEGIN EXPLICIT-FREE-LIST FUNCTIONS
 * ---------------------------------------------------------------------------
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
 * @param[in] head
 * @param[in] b
 * @return
 */
static void insert_head(block_t **head, block_t *b) {
    dbg_requires(head != NULL);
    dbg_requires(b != NULL);
    dbg_requires(!get_alloc(b));
    dbg_requires(!is_in(head, b));

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
}

/**
 * @brief Removes an element from the head of the list
 * @param[in] head
 * @return the removed element
 */
static block_t *remove_head(block_t **head) {
    dbg_requires(head != NULL);
    dbg_requires(*head != NULL);

    block_t *b = (*head);

    if ((*head)->un.node.next == NULL) {
        (*head) = NULL;
    } else {
        (*head) = (*head)->un.node.next;
        (*head)->un.node.prev = NULL;
    }
    b->un.node.next = NULL;

    dbg_ensures(!is_in(head, b));
    return b;
}

/**
 * @brief Splices out an element the list
 * Also, sets the prev and next pointers
 * which are used to splice in a free block
 * to the position that was previously occupied
 * by b.
 * @param[in] head
 * @param[in] b - block to splice out
 * @param[in] prev - sets this to what b->prev points to
 * @param[in] next - sets this to what b->next points to
 * @return the removed element
 */
static void remove_block(block_t **head, block_t *b, block_t **prev,
                         block_t **next) {
    dbg_requires(head != NULL);
    dbg_requires(*head != NULL);
    dbg_requires(b != NULL);
    dbg_requires(is_in(head, b));

    if ((*head) == b) {
        (*next) = b->un.node.next;
        (*prev) = NULL;
        remove_head(head);
    } else {
        (*next) = b->un.node.next;
        (*prev) = b->un.node.prev;

        if ((*prev) == NULL && (*next) == NULL)
            return;

        if ((*prev) != NULL) {
            (*prev)->un.node.next = (*next);
        }
        if ((*next) != NULL) {
            (*next)->un.node.prev = (*prev);
        }
    }

    b->un.node.prev = NULL;
    b->un.node.next = NULL;
    dbg_ensures(!is_in(head, b));
}

/**
 * @brief Splices in an element to the list
 * right between prev and next.
 * @param[in] head
 * @param[in] b - block to splice out
 * @param[in] prev - sets this to what b->prev points to
 * @param[in] next - sets this to what b->next points to
 * @return the removed element
 */
static void insert_block(block_t **head, block_t *new_b, block_t **prev,
                         block_t **next) {
    dbg_assert(head != NULL);
    dbg_assert(new_b != NULL);
    dbg_requires(!is_in(head, new_b));

    if ((*prev) == NULL) {
        insert_head(head, new_b);
    } else {

        (*prev)->un.node.next = new_b;
        new_b->un.node.prev = (*prev);

        if ((*next) != NULL) {
            (*next)->un.node.prev = new_b;
            new_b->un.node.next = (*next);
        }
    }

    dbg_ensures(is_in(head, new_b));
    dbg_ensures(!(next == NULL && prev == NULL));
}

/*
 * ---------------------------------------------------------------------------
 *                        END EXPLICIT-FREE-LIST HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] block
 * @return
 */
static block_t *coalesce_block(block_t *block) {
    dbg_requires(block != NULL);

    block_t *prev_block = find_prev(block);
    block_t *next_block = find_next(block);
    bool prev_alloc = get_alloc(prev_block);
    bool next_alloc = get_alloc(next_block);
    size_t curr_size = get_size(block);

    block_t *prev;
    block_t *next;

    if (prev_alloc && next_alloc) { /* Case 1: Prev and Next are allocated */
        insert_head(&explicit_free_list, block);
        return block;
    } else if (prev_alloc && !next_alloc) /* Case 2: Next is free */
    {
        /// @NEW: Splice out Adjacent block

        remove_block(&explicit_free_list, next_block, &prev, &next);
        curr_size += get_size(next_block);
        write_block(block, curr_size, false);
        insert_head(&explicit_free_list, block);
        return block;
    } else if (!prev_alloc && next_alloc) /* Case 3: Prev is free */
    {
        /// @NEW: Splice out prev block
        remove_block(&explicit_free_list, prev_block, &prev, &next);
        curr_size += get_size(prev_block);
        write_block(prev_block, curr_size, false);
        insert_head(&explicit_free_list, prev_block);
        return prev_block;
    } else /* Prev and Next are free */
    {
        /// @NEW: Splice out prev-next blocks
        remove_block(&explicit_free_list, prev_block, &prev, &next);
        remove_block(&explicit_free_list, next_block, &prev, &next);
        curr_size += get_size(prev_block) + get_size(next_block);
        write_block(prev_block, curr_size, false);
        insert_head(&explicit_free_list, prev_block);
        return prev_block;
    }
}

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] size
 * @return
 */
static block_t *extend_heap(size_t size) {
    void *bp;

    // Allocate an even number of words to maintain alignment
    size = round_up(size, dsize);
    if ((bp = mem_sbrk((intptr_t)size)) == (void *)-1) {
        return NULL;
    }

    /*
     * TODO: delete or replace this comment once you've thought about it.
     * Think about what bp represents. Why do we write the new block
     * starting one word BEFORE bp, but with the same size that we
     * originally requested?
     * bp represents the first byte of the newly allocated heap area.
     */

    // Initialize free block header/footer
    block_t *block = payload_to_header(bp);
    write_block(block, size, false);

    // Create new epilogue header
    block_t *block_next = find_next(block);
    write_epilogue(block_next);

    // Coalesce in case the previous block was free
    /// @DONE: This takes care of adding the free block to the
    /// explicit-free-list
    block = coalesce_block(block);

    return block;
}

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] block
 * @param[in] asize
 */
static void split_block(block_t *block, size_t asize) {
    dbg_requires(get_alloc(block));
    /* TODO: Can you write a precondition about the value of asize? */
    dbg_requires(asize <= get_size(block));

    size_t block_size = get_size(block);

    block_t *prev = NULL;
    block_t *next = NULL;

    remove_block(&explicit_free_list, block, &prev, &next);

    if ((block_size - asize) >= min_block_size) {
        block_t *block_next;
        write_block(block, asize, true);
        block_next = find_next(block);
        write_block(block_next, block_size - asize, false);
        insert_head(&explicit_free_list, block_next);
    }
    dbg_ensures(get_alloc(block));
}

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] asize
 * @return
 */
static block_t *find_fit(size_t asize) {

    for (block_t *block = explicit_free_list; block != NULL;
         block = block->un.node.next) {
        if (asize <= get_size(block))
            return block;
    }
    return NULL; // No fit found
}

static void print_list() {
    printf("| HEAD |\n");
    size_t nblocks = 0;
    size_t tsize = 0;
    for (block_t *block = explicit_free_list; block != NULL;
         block = block->un.node.next) {
        printf("|i : %zu, alloc: %d, size: %zu |\n", nblocks, get_alloc(block),
               get_size(block));
        nblocks++;
        tsize += get_size(block);
    }
    printf("|nblocks: %zu, tsize: %zu|\n", nblocks, tsize);
}

/**
 * @brief
 *
 * Checks a block for memory alighment
 * and matching header-footer pairs
 *
 * @param[in] block
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

    /* Check if header matches footer */
    if (get_size(block) != extract_size(*header_to_footer(block))) {
        dbg_printf("Error: Footer size does not match Header %p\n",
                   (void *)block);
        return false;
    }

    if (get_alloc(block) != extract_alloc(*header_to_footer(block))) {
        dbg_printf("Error: Footer alloc does not match Header %p\n",
                   (void *)block);
        return false;
    }

    /* If block is free, check if the preceding and following blocks are
     * allocated */
    if (!get_alloc(block)) {
        bool prev_alloc = get_alloc(find_prev(block));
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
 * Checks list for memory alignment
 * and matching next-prev pointers
 *
 * @param[in] block
 */
static bool check_free_list(size_t *num_blocks, size_t *total_size, int line) {

    for (block_t *s = explicit_free_list; s != NULL; s = s->un.node.next) {
        if (get_alloc(s)) {
            dbg_printf("Error: Bad alloc, should be free && Line: %d\n", line);
            return false;
        }

        /* Check within heap boundaries */
        if (!(((size_t)s > (size_t)mem_heap_lo() &&
               (size_t)s < (size_t)mem_heap_hi()))) {
            dbg_printf("Error: Out of heap boundaries && Line: %d\n", line);
            return false;
        }

        block_t *f = s->un.node.next;
        if (f != NULL) {
            if (s != f->un.node.prev) {
                dbg_printf("Error: f->prev != s && Line: %d\n", line);
                return false;
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
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] line
 * @return
 */
bool mm_checkheap(int line) {
    /*
     * TODO: Delete this comment!
     *
     * You will need to write the heap checker yourself.
     * Please keep modularity in mind when you're writing the heap checker!
     *
     * As a filler: one guacamole is equal to 6.02214086 x 10**23 guacas.
     * One might even call it...  the avocado's number.
     *
     * Internal use only: If you mix guacamole on your bibimbap,
     * do you eat it with a pair of chopsticks, or with a spoon?
     */

    // Check Prologue
    block_t *prologue = find_prev(heap_start);

    if (get_size(prologue) != 0 || !get_alloc(prologue)) {
        dbg_printf("Error: Bad Prologue && Line: %d\n", line);
        return false;
    }

    block_t *block;

    size_t num_free_blocks = 0;
    size_t free_size = 0;
    // Traverse the Blocks
    for (block = heap_start; get_size(block) > 0; block = find_next(block)) {
        if (!check_block(block))
            return false;

        if (!get_alloc(block)) {
            num_free_blocks++;
            free_size += get_size(block);
        }
    }

    size_t nblocks = 0;
    size_t tsize = 0;

    if (!check_free_list(&nblocks, &free_size, line))
        return false;

    /// @todo: Verify size matches
    if (num_free_blocks != nblocks && free_size != tsize) {
        dbg_printf("Error: Number|Size mismatch between explicit-free-lists "
                   "and heap && Line: %d\n",
                   line);
        return false;
    }

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
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @return
 */
bool mm_init(void) {
    // Create the initial empty heap
    word_t *start = (word_t *)(mem_sbrk(2 * wsize));

    if (start == (void *)-1) {
        return false;
    }

    /*
     * TODO: delete or replace this comment once you've thought about it.
     * Think about why we need a heap prologue and epilogue. Why do
     * they correspond to a block footer and header respectively?
     */

    start[0] = pack(0, true); // Heap prologue (block footer)
    start[1] = pack(0, true); // Heap epilogue (block header)

    // Heap starts with first "block header", currently the epilogue
    heap_start = (block_t *)&(start[1]);

    // Initialize Explicit Free List
    explicit_free_list = NULL;

    // Extend the empty heap with a free block of chunksize bytes
    /// @DONE: Add the free block to explicit-free-list
    /// Handled by call to coalesce within extend_heap
    if (extend_heap(chunksize) == NULL) {
        return false;
    }

    return true;
}

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] size
 * @return
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
    asize = round_up(size + dsize, dsize);

    // Search the free list for a fit
    /// @TODO: Search from free blocks in explicit free list
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
    write_block(block, block_size, true);
    // Try to split the block if too large
    /// @DONE: If there is a split, add the free split to explicit-free-list
    split_block(block, asize);
    bp = header_to_payload(block);

    dbg_ensures(mm_checkheap(__LINE__));
    return bp;
}

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] bp
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
    write_block(block, size, false);

    // Try to coalesce the block with its neighbors
    /// @DONE: If blocks are coalesced, add the final free block to
    /// explicit-free-list
    coalesce_block(block);

    dbg_ensures(mm_checkheap(__LINE__));
}

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] ptr
 * @param[in] size
 * @return
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
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] elements
 * @param[in] size
 * @return
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
