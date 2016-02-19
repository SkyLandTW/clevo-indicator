vpath %.c ../src

CC = gcc
CFLAGS = -c -Wall
LDFLAGS =

DSTDIR := /usr/local
OBJDIR := obj
SRCDIR := src

SRC = clevo-indicator.c
OBJ = $(patsubst %.c,$(OBJDIR)/%.o,$(SRC)) 

TARGET = bin/clevo-indicator

CFLAGS += `pkg-config --cflags appindicator3-0.1`
LDFLAGS += `pkg-config --libs appindicator3-0.1`

all: $(TARGET)

install: $(TARGET)
	@echo Install to ${DSTDIR}/bin/
	@sudo install -m 4750 -g adm $(TARGET) ${DSTDIR}/bin/

test: $(TARGET)
	@sudo chown root $(TARGET)
	@sudo chgrp adm  $(TARGET)
	@sudo chmod 4750 $(TARGET)

$(TARGET): Makefile $(OBJ)
	@mkdir -p bin
	@echo linking $(TARGET) from $(OBJ)
	@$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS) -lm

clean:
	rm $(OBJ) $(TARGET)

$(OBJDIR)/%.o : $(SRCDIR)/%.c
	@echo compiling $< 
	@$(CC) $(CFLAGS) -c $< -o $@

#$(OBJECTS): | obj

#obj:
#	@mkdir -p $@
