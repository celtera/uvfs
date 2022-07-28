# uvfs

Microscopic archive format optimized for fast indexing and memory-mapping of data.

# Unscientific benchmark

- CPU: i7-6900k
- Given 98k files in my /usr/include 
- This gives me a 771M .uvfs file
- Loading that file takes around 4400us.
- Getting a pointer to every single file in the loaded archive, in a random order when compared to the index takes around 2900us.

