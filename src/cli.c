#include "cli.h"

#include <stdio.h>
#include <string.h>

enum {
    STATUS_OK = 0,
    STATUS_INVALID_ARGS = 1
};

static void init_options(CliOptions *options) {
    options->db_dir = NULL;
    options->sql_file = NULL;
    options->help_requested = 0;
}

void print_usage(const char *program_name) {
    printf("Usage: %s -d <db_dir> -f <sql_file>\n", program_name);
    printf("       %s --db <db_dir> --file <sql_file>\n", program_name);
    printf("       %s -h | --help\n", program_name);
}

int parse_cli_args(int argc, char **argv, CliOptions *out_options) {
    int i;

    if (out_options == NULL) {
        return STATUS_INVALID_ARGS;
    }

    init_options(out_options);

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            out_options->help_requested = 1;
            continue;
        }

        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--db") == 0) {
            if (i + 1 >= argc) {
                return STATUS_INVALID_ARGS;
            }
            out_options->db_dir = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) {
            if (i + 1 >= argc) {
                return STATUS_INVALID_ARGS;
            }
            out_options->sql_file = argv[++i];
            continue;
        }

        return STATUS_INVALID_ARGS;
    }

    if (out_options->help_requested) {
        return STATUS_OK;
    }

    if (out_options->db_dir == NULL || out_options->sql_file == NULL) {
        return STATUS_INVALID_ARGS;
    }

    return STATUS_OK;
}
