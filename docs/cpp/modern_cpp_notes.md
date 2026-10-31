# Modern C++ notes

## RAII
Tie a resource to an object lifetime. The destructor releases it, so early returns and exceptions cannot leak it.
Use std::unique_ptr for single ownership and std::shared_ptr only when ownership is really shared.

## Threads
std::thread runs a function on another thread, and it must be joined or detached before it is destroyed.
Protect shared data with std::mutex and lock it with std::lock_guard. std::atomic is for simple flags and counters.
std::async with std::launch::async runs work in the background and returns a std::future.

## const and references
Pass large objects by const reference. Mark member functions const when they do not change the object.

## CMake basics
add_executable builds a program, add_library builds a library, target_link_libraries connects them.
Use target_include_directories for header paths. Set CMAKE_CXX_STANDARD to 17 to enable C++17.
