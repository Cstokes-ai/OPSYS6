CC      = gcc -g3
CFLAGS  = -g3
TARGET1 = oss
TARGET2 = user

OBJS1   = oss.o
OBJS2   = user.o

# Default target to build both programs
all: $(TARGET1) $(TARGET2)

# Rule to build oss
$(TARGET1): $(OBJS1)
	$(CC) -o $(TARGET1) $(OBJS1)

# Rule to build user
$(TARGET2): $(OBJS2)
	$(CC) -o $(TARGET2) $(OBJS2)

# Compile oss source file
oss.o: oss.c
	$(CC) $(CFLAGS) -c oss.c

# Compile user source file
user.o: user.c
	$(CC) $(CFLAGS) -c user.c

# Clean up object files and executables
clean:
	/bin/rm -f *.o $(TARGET1) $(TARGET2)
