int num_out();

int sort(int a[]);

int main() {
    int a[4] = {5, 2, 3, 4};
    sort(a);
}

int sort(int a[]) {
    int n, i, k;
    for (k = 0; k < 4; k++)
        for (i = 0; i < 4 - k; i++)
            if (a[i] > a[i + 1]) {
                n = a[i + 1];
                a[i + 1] = a[i];
                a[i] = n;
            }
}
