#
# test directory makefile
#

FNAME        := Eig_gest

SRC_PATH     := ../..
OBJ_PATH     := .

INST_PATH    := ~/Desktop/libflame
LIB_PATH     := $(INST_PATH)/lib/x86_64-unknown-linux-gnu
INC_PATH     := $(INST_PATH)/include/x86_64-unknown-linux-gnu
FLAME_LIB    := $(LIB_PATH)/libflame.so
#BLAS_LIB     := $(LIB_PATH)/libblas.a
#BLAS_LIB     := $(LIB_PATH)/libgoto.a
#BLAS_LIB     := $(LIB_PATH)/liblapack.a $(LIB_PATH)/libgoto.a
BLAS_LIB     := -lopenblas
HELPER_LIB   := /home/joey/Desktop/libflame-hiphopper-main/build/library/src/libflame-hiphopper.so



CC           := gcc
FC           := gfortran
LINKER       := $(CC)
#CFLAGS       := -I$(SRC_PATH) -I$(INC_PATH) -g -O3 -Wall -Wno-comment
CFLAGS       := -I $(SRC_PATH) -I $(INC_PATH) -I /opt/rocm/include -g -O3 -Wall -Wno-comment

FFLAGS       := $(CFLAGS)
#LDFLAGS      := -L/lusr/opt/gcc-4.2.2/lib/gcc/i686-pc-linux-gnu/4.2.2 -L/lusr/opt/gcc-4.2.2/lib/gcc/i686-pc-linux-gnu/4.2.2/../../.. -lgfortranbegin -lgfortran -lm
LDFLAGS      := -lm 
LDFLAGS      += -lpthread

TEST_OBJS    := $(patsubst $(SRC_PATH)/%.c, $(OBJ_PATH)/%.o, $(wildcard $(SRC_PATH)/*.c))
TEST_OBJS    += test_$(FNAME).o \
                time_$(FNAME).o \
                REF_$(FNAME).o
TEST_BIN     := test_$(FNAME).x

$(OBJ_PATH)/%.o: $(SRC_PATH)/%.c
	$(CC) $(CFLAGS) -c $< -o $@
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test_$(FNAME): $(TEST_OBJS)
	$(LINKER) $(TEST_OBJS) $(LDFLAGS) $(FLAME_LIB) $(LAPACK_LIB) $(BLAS_LIB) $(HELPER_LIB) -o $(TEST_BIN)

clean:
	rm -f *.o $(OBJ_PATH)/*.o *~ core *.x
check:

	ls ${INC_PATH}
	echo ${CFLAGS}
