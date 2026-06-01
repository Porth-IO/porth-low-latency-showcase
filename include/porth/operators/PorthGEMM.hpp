
template <size_t TILE_SIZE>
void GEMM(const float* __restrict__ A, const float* __restrict__ B, float* __restrict__ C, 
                int M, int N, int K) {

    alignas(64) float scratch[TILE_SIZE][TILE_SIZE];

    for (int ii = 0; ii < M; ii += TILE_SIZE) {
        int i_limit = std::min(ii + TILE_SIZE, M);

        for (int jj = 0; jj < N; jj += TILE_SIZE) {
            int j_limit = std::min(jj + TILE_SIZE, N);

            for (int i = ii; i < i_limit; ++i) {
                int local_i = i - ii;

                for (int j = jj; j < j_limit; ++j) {
                    scratch[local_i][j - jj] = 0.0f;
                }
            }

            for (int kk = 0; kk < K; kk += TILE_SIZE) {
                int k_limit = std::min(kk + TILE_SIZE, K);

                for (int i = ii; i < i_limit; ++i) {
                    int local_i = i - ii;
                    for (int k = kk; k < k_limit; ++k) {
                        float a_val = A[i * K + k];

                        #pragma omp simd
                        for (int j = jj; j < j_limit; ++j) {
                            scratch[local_i][j - jj] += a_val * B[k * N + j];
                        }
                    }
                }
            }

            for (int i = ii; i < i_limit; ++i) {
                int local_i = i - ii;

                #pragma omp simd
                for (int j = jj; j < j_limit; ++j) {
                    C[i * N + j] = scratch[local_i][j - jj];
                }
            }
        }
    }
}
