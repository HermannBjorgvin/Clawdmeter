#pragma once

inline int compute_splash_cell(int minimum_dimension, bool has_psram) {
    int cell = minimum_dimension / 20;
    if (cell < 4) {
        cell = 4;
    }

    if (!has_psram) {
        const int maximum_cell = minimum_dimension <= 240 ? 8 : 10;
        if (cell > maximum_cell) {
            cell = maximum_cell;
        }
    }

    return cell;
}
