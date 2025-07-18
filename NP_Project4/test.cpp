#include <stdio.h> 
#include <stdlib.h> 
#include <time.h>
#include <omp.h>
#define N 50001

// 平行運算 Monte Carlo 估算函數
int parallel_monte_carlo(int num_points) {
    int sum = 0;
    int i;
    
    // 設定隨機數種子，每個執行緒使用不同種子
    #pragma omp parallel
    {
        int thread_id = omp_get_thread_num();
        srand(time(NULL) + thread_id);
    }
    
    // 平行化迴圈，使用 reduction 來安全地累加 sum
    #pragma omp parallel for reduction(+:sum) private(i)
    for(i = 1; i < num_points; i++) { 
        double x = (double) rand() / RAND_MAX; 
        double y = (double) rand() / RAND_MAX; 
        if((x * x + y * y) < 1) {
            sum++; 
        }
    }
    
    return sum;
}

int main(void) { 
    double start_time, end_time;
    start_time = omp_get_wtime();
    
    // 呼叫平行運算函數
    int sum = parallel_monte_carlo(N);
    
    end_time = omp_get_wtime();
    
    printf("執行緒數量: %d\n", omp_get_max_threads());
    printf("PI = %f\n", (double) 4 * sum / (N - 1));
    printf("執行時間: %f 秒\n", end_time - start_time);
    
    return 0; 
}