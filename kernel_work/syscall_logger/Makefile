# Makefile for the syscall_logger kernel module

# The name of the module object file
obj-m += syscall_logger.o

# The 'all' target is the default. It tells make to change directory
# to the kernel's build directory and then build the 'modules' target
# from there. The 'M=$(PWD)' tells kbuild where our module source is.
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

# The 'clean' target removes all generated files.
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
