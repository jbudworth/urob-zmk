/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr.h>
#include <zmk/matrix_transform.h>
#include <zmk/matrix.h>
#include <dt-bindings/zmk/matrix_transform.h>

#ifdef ZMK_KEYMAP_TRANSFORM_NODE

#define INDEX_OFFSET 1

#define TRANSFORM_ENTRY(i, _)                                                                     \
    [(KT_ROW(DT_PROP_BY_IDX(ZMK_KEYMAP_TRANSFORM_NODE, map, i)) * ZMK_MATRIX_COLS) +              \
        KT_COL(DT_PROP_BY_IDX(ZMK_KEYMAP_TRANSFORM_NODE, map, i))] = i + INDEX_OFFSET,

static uint32_t transform[] = {UTIL_LISTIFY(ZMK_KEYMAP_LEN, TRANSFORM_ENTRY, 0)};

#endif

int32_t zmk_matrix_transform_row_column_to_position(uint32_t row, uint32_t column) {
#if DT_NODE_HAS_PROP(ZMK_KEYMAP_TRANSFORM_NODE, col_offset)
    column += DT_PROP(ZMK_KEYMAP_TRANSFORM_NODE, col_offset);
#endif

#if DT_NODE_HAS_PROP(ZMK_KEYMAP_TRANSFORM_NODE, row_offset)
    row += DT_PROP(ZMK_KEYMAP_TRANSFORM_NODE, row_offset);
#endif

    const uint32_t matrix_index = (row * ZMK_MATRIX_COLS) + column;

#ifdef ZMK_KEYMAP_TRANSFORM_NODE
    if (matrix_index >= ARRAY_SIZE(transform)) {
        return -EINVAL;
    }

    const uint32_t value = transform[matrix_index];

    if (!value) {
        return -EINVAL;
    }

    return value - INDEX_OFFSET;
#else
    return matrix_index;
#endif /* ZMK_KEYMAP_TRANSFORM_NODE */
};
