vpath %.c ../src

CC = gcc
CFLAGS = -c -Wall
LDFLAGS =

OBJDIR := obj
SRCDIR := src

SRC = clevo-indicator.c
OBJ = $(patsubst %.c,$(OBJDIR)/%.o,$(SRC)) 

TARGET = bin/clevo-indicator

CFLAGS += `pkg-config --cflags appindicator3-0.1`
LDFLAGS += `pkg-config --libs appindicator3-0.1`

all: $(TARGET)

test: $(TARGET)
	@sudo chown root $(TARGET)
	@sudo chmod u+s $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p bin
	@echo $(TARGET) from $(OBJ)
	@$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS) -lm

clean:
	rm $(OBJ) $(TARGET)

$(OBJDIR)/%.o : $(SRCDIR)/%.c
	@echo $< 
	@$(CC) $(CFLAGS) -c $< -o $@

#$(OBJECTS): | obj

#obj:
#	@mkdir -p $@
