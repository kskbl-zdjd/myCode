# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "")
  file(REMOVE_RECURSE
  "AlgorithmVisualizer_autogen"
  "CMakeFiles\\AlgorithmVisualizer_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\AlgorithmVisualizer_autogen.dir\\ParseCache.txt"
  )
endif()
