#ifndef CODE_EXAMPLES_H
#define CODE_EXAMPLES_H

#include <QString>

namespace CodeExamples {

inline const QString BUBBLE_SORT = R"cpp(
void bubbleSort(int arr[], int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                // 交换 arr[j] 和 arr[j+1]
                int temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
        // 第 i 轮结束，arr[n-1-i] 已就位
    }
}
)cpp";

inline const QString SELECTION_SORT = R"cpp(
void selectionSort(int arr[], int n) {
    for (int i = 0; i < n - 1; i++) {
        int minIdx = i;
        for (int j = i + 1; j < n; j++) {
            if (arr[j] < arr[minIdx]) {
                minIdx = j;
            }
        }
        // 交换 arr[i] 和 arr[minIdx]
        int temp = arr[i];
        arr[i] = arr[minIdx];
        arr[minIdx] = temp;
        // arr[i] 已就位
    }
}
)cpp";

inline const QString INSERTION_SORT = R"cpp(
void insertionSort(int arr[], int n) {
    for (int i = 1; i < n; i++) {
        int key = arr[i];
        int j = i - 1;
        // 将 key 插入到有序区间的正确位置
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];  // 后移
            j--;
        }
        arr[j + 1] = key;  // 插入
    }
}
)cpp";

inline const QString QUICK_SORT = R"cpp(
void quickSort(int arr[], int low, int high) {
    if (low < high) {
        int pi = partition(arr, low, high);
        quickSort(arr, low, pi - 1);
        quickSort(arr, pi + 1, high);
    }
}

int partition(int arr[], int low, int high) {
    int pivot = arr[high];  // 选择基准
    int i = low - 1;
    for (int j = low; j < high; j++) {
        if (arr[j] < pivot) {
            i++;
            // 交换 arr[i] 和 arr[j]
            int temp = arr[i];
            arr[i] = arr[j];
            arr[j] = temp;
        }
    }
    // 基准归位
    int temp = arr[i + 1];
    arr[i + 1] = arr[high];
    arr[high] = temp;
    return i + 1;
}
)cpp";

} // namespace CodeExamples

#endif // CODE_EXAMPLES_H
