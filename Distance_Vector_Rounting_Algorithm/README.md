# Assignment 2

## Distance Vector Routing on Virtual Network
See the assignment specification.

We **highly** recommend you read both the spec and README.md (this file) in full before you start!

We also **highly** recommend you to review the DV algorithm from the early lectures. It may help to hand-trace the algorithm on a small network.

You may reuse any code from assignments 0 and 1 as well.

## DUE
**October 7, 11:59:59 PM ET**

## Piazza
Please tag any questions pertaining to this assignment with `a2`.

---
## Project Structure

### A TA's attempt at explaining this 30-year old code
We have partially restructured the sample code so you can spend less time deciphering existing code.

All the parts you need to modify are marked with `TODO:`, with some hints.

`dv.c` is the file where you will implement your DV algorithm. You will have to modify most (if not all) of this file.

For the `ls.c`, you will only have to implement `create_link_sock()` function.

---
Below is some explanation for other files. You don't have to touch these:

The entrypoint is on `dr.c` - the `main()` function parses arguments, parses `config` file, initializes global routing table variables, and start executing events using the `walk_event_set_list()` function.

The `walk_event_set_list()` (in `dv.c`) does things on the "list of [event sets]". (i.e. this "list" is 2-d)

`rt.h` and `ls.h` contains definitions and helper functions for `routing table` data structure and `link` data structure, respectively.

You may have a compiler warning for `lex.ru.c`. It is safe to ignore this.

---

### FILES
```
dv.*	 :: your code goes here
ru.*	 :: parser and scanner 
es.*	 :: event set 
ls.*	 :: link set 
rt.*	 :: routing table 
n2h.*	 :: node-to-hostname 
dr.c	 :: a testing driver, including main(), calls walk_event_set_list()
common.h :: common definitions
queue.h	 :: queue operation definition and macros
makefile :: type 'make' to generate executable "rt"
config	 :: a sample scenario file
```
