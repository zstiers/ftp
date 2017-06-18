# FTP
FTP is a header only C++ Fast Thread Pool implementation.

## Features
  - Faster than anything I could find
  - Full C\++11 compliance and depends only on C++11 compliant headers
  - Header only and depends only on headers allowing easy integration
  - No limit on the number of queued tasks
  - Pool can be dynamically resized
  - Supports triggering callbacks on thread start and stop
  - Code is easy to read
  - Permissive liscensing on this and all dependent libraries
  
## Remaining Work
  - Extensive testing
  - Expose control over dynamic memory allocations
  - Provide access to the better performing internal queue functions

## Reasons to use
You have many tasks that can be run over multiple threads and don't want to write scheduling code yourself and would instead just push tasks into a prebuilt pool.

## Reasons not to use
You are looking for something mature and fully tested. If you are willing to notify me of found issues I will try to resolve any problems though.

## Example usage
```c++
ftp::ThreadPool<std::function<void()>> pool(4); // Make a pool with 4 threads.
pool.Push([] () { printf("Hello World!\n"); });
```

## Credits
  - [moodycamel::ConcurrentQueue] is used internally. The header is included and must copied as part of bringing this into your project.
  - [CTPL] was used as inspiration in writing this
  
  [CTPL]: https://github.com/vit-vit/CTPL
  [moodycamel::ConcurrentQueue]: https://github.com/cameron314/concurrentqueue
