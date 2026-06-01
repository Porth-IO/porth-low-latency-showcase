void softmax(const float* X, float* Y, int N) {
    float max_val = -INFINITY;
    float sum = 0.0f;

    for (int i = 0; i < N; ++i) {
        float x_val = X[i];

        if (x_val > max_val) {
            sum = sum * std::exp(max_val - x_val) + 1.0f;
            max_val = x_val;
        } else {
            sum += std::exp(x_val - max_val);
        }
    }

    float inv_sum = 1.0f / sum;

    #pragma omp simd
    for (int i = 0; i < N; ++i) {
        Y[i] = std::exp(X[i] - max_val) * inv_sum;
    }
}