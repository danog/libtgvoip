# Daniil Gentili's submission to the VoIP contest 2

My submission consists of a major refactor (to C++) of the existing libtgvoip library.

The existing code was somewhat readable, even if extremely bulky, all in one single controller class, and had way too many C-isms, assertions, unmanaged pointers and then unoptimized C++ code progressively added into the library at later stages.

I've removed many C-isms within the main controller, switching entirely to smart pointers for the management of internal objects, reducing to 0 the number of `delete`s in the destructor; with multiple refactoring passes I've optimized many places of the network loop with modern C++ data structures and general logical optimizations.
I've also applied several optimizations to the code of the controller and helper utilities especially in the MessageThread, buffers and main network threads, switching to the C++ STL for many otherwise highly unefficient and unsafe operations (like throwing assertions if the `delet`ion order of objects using buffers from bufferpool isn't exactly right, easily fixed with smart pointers and a lambda).



