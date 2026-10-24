static int sum_array(const int *values, int count)
{
    int sum = 0;

    for (int i = 0; i < count; i++) {
        sum += values[i];
    }

    return sum;
}

static void print_int(int value)
{
    register int a0 asm("a0") = value;
    register int a7 asm("a7") = 1;

    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

int main(void)
{
    int values[] = {3, 4, 5, 6, 7, 8, 9};

    int result = sum_array(values, 7);
    print_int(result);

    return 0;
}
