/* misc_tools.h
 *
 * Miscellaneous functions and data structures for doing random things.
 *
 *  Copyright (C) 2013 Tox project All Rights Reserved.
 *
 *  This file is part of Tox.
 *
 *  Tox is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Tox is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef MISC_TOOLS_H
#define MISC_TOOLS_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h> /* for memcpy() */

unsigned char *hex_string_to_bin(char hex_string[]);

/*********************Debugging Macros********************
 * wiki.tox.im/index.php/Internal_functions_and_data_structures#Debugging
 *********************************************************/
#ifdef DEBUG
    #include <assert.h>
    #include <stdio.h>
    #include <string.h>

    #define DEBUG_PRINT(str, ...) do { \
        char msg[1000]; \
        sprintf(msg, "%s(): line %d (file %s): %s%%c\n", __FUNCTION__, __LINE__, __FILE__, str); \
        fprintf(stderr, msg, __VA_ARGS__); \
    } while (0)

    #define WARNING(...) do { \
        fprintf(stderr, "warning in "); \
        DEBUG_PRINT(__VA_ARGS__, ' '); \
    } while (0)

    #define INFO(...) do { \
        DEBUG_PRINT(__VA_ARGS__, ' '); \
    } while (0)

    #undef ERROR
    #define ERROR(exit_status, ...) do { \
        fprintf(stderr, "error in "); \
        DEBUG_PRINT(__VA_ARGS__, ' '); \
        exit(exit_status); \
    } while (0)
#else
    #define WARNING(...)
    #define INFO(...)
    #undef ERROR
    #define ERROR(...)
#endif // DEBUG

/************************Linked List***********************
 * http://wiki.tox.im/index.php/Internal_functions_and_data_structures#Linked_List
 * TODO: Update wiki.
 **********************************************************/

#define MEMBER_OFFSET(var_name_in_parent, parent_type) \
   (&(((parent_type*)0)->var_name_in_parent))

#define GET_PARENT(var, var_name_in_parent, parent_type) \
   ((parent_type*)((uint64_t)(&(var)) - (uint64_t)(MEMBER_OFFSET(var_name_in_parent, parent_type))))

#define TOX_LIST_FOR_EACH(lst, tmp_name) \
   for (tox_list* tmp_name = lst.next; tmp_name != &lst; tmp_name = tmp_name->next)

#define TOX_LIST_GET_VALUE(tmp_name, name_in_parent, parent_type) GET_PARENT(tmp_name, name_in_parent, parent_type)

typedef struct tox_list {
    struct tox_list *prev, *next;
} tox_list;

/* Returns a new tox_list_t. */
static inline void tox_list_new(tox_list *lst)
{
    lst->prev = lst->next = lst;
}

/* Inserts a new tox_lst after lst and returns it. */
static inline void tox_list_add(tox_list *lst, tox_list *new_lst)
{
    tox_list_new(new_lst);

    new_lst->next = lst->next;
    new_lst->next->prev = new_lst;

    lst->next = new_lst;
    new_lst->prev = lst;
}

static inline void tox_list_remove(tox_list *lst)
{
    lst->prev->next = lst->next;
    lst->next->prev = lst->prev;
}

/****************************Array***************************
 * Array which manages its own memory allocation.
 * It stores copy of data (not pointers).
 * TODO: Add wiki info usage.
 ************************************************************/

typedef struct tox_array {
    void *data;
    uint32_t len;
    size_t elem_size; /* in bytes */
} tox_array;

static inline void tox_array_init(tox_array *arr, size_t elem_size)
{
    arr->len = 0;
    arr->elem_size = elem_size;
    arr->data = NULL;
}

static inline void tox_array_delete(tox_array *arr)
{
    free(arr->data);
    arr->len = arr->elem_size = 0;
}

static inline void _tox_array_push(tox_array *arr, uint8_t *item)
{
    arr->data = realloc(arr->data, arr->elem_size * (arr->len+1));

    memcpy(arr->data + arr->elem_size*arr->len, item, arr->elem_size);
    arr->len++;
}
#define tox_array_push(arr, item) _tox_array_push(arr, (void*)(&(item)))

/* Deletes num items from array.
 * Not same as pop in stacks, because to access elements you use data.
 */
static inline void tox_array_pop(tox_array *arr, uint32_t num)
{
    arr->len--;
    arr->data = realloc(arr->data, arr->elem_size*arr->len);
}

#define tox_array_get(arr, i, type) ((type*)(arr)->data)[i]

#endif // MISC_TOOLS_H
