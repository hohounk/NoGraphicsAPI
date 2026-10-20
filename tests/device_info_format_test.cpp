// Checks the output helpers of the device_info example without a GPU.
#define main device_info_example_main
#include "../examples/device_info/device_info.cpp"
#undef main

#include <string.h>

namespace
{
uint32 failures = 0;

void check(const char* output, const char* expected, const char* message) noexcept
{
    if (strstr(output, expected)) return;
    fprintf(stderr, "FAIL: %s: expected \"%s\"\n", message, expected);
    ++failures;
}
}

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    // Captures stdout in a file; failures go to stderr.
    if (!freopen(argv[1], "w", stdout)) return 1;
    print_bytes("a", 1023ull);
    print_bytes("b", 1024ull);
    print_bytes("c", 1024ull * 1024ull);
    print_bytes("d", 1024ull * 1024ull * 1024ull);
    print_api_version((1u << 29u) | (1u << 22u) | (4u << 12u) | 7u);
    fclose(stdout);

    FILE* file = fopen(argv[1], "r");
    if (!file) return 1;
    char output[4096]{};
    output[fread(output, 1, sizeof(output) - 1, file)] = '\0';
    fclose(file);

    check(output, "1023 bytes\n", "1023 bytes stays in bytes");
    check(output, "1024 bytes (1.00 KiB)", "1024 bytes prints as KiB");
    check(output, "1048576 bytes (1.00 MiB)", "1 MiB prints as MiB");
    check(output, "1073741824 bytes (1.00 GiB)", "1 GiB prints as GiB");
    check(output, "(1.4.7)", "the API major version masks out the variant bits");
    return failures ? 1 : 0;
}
