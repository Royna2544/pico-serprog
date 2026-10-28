add_library(pico-mock-sdk src/dummy.c src/irq.c src/queue.cpp)
target_include_directories(pico-mock-sdk PUBLIC inc)
target_link_libraries(pico-mock-sdk PUBLIC boardstate-client)