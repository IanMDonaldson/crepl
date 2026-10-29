CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra
PREFIX  ?= $(HOME)/.local

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  EXPORT := -Wl,-export_dynamic
  LIBS   :=
else
  EXPORT := -rdynamic
  LIBS   := -ldl
endif

RLTEST := $(shell printf '#include <stdio.h>\n#include <readline/readline.h>\nint main(void){return 0;}\n' > .rltest.c 2>/dev/null; \
                  $(CC) .rltest.c -lreadline -o .rltest.bin >/dev/null 2>&1 && echo yes || echo no; \
                  rm -f .rltest.c .rltest.bin)
ifeq ($(RLTEST),yes)
  RL_CFLAGS := -DCREPL_READLINE
  RL_LIBS   := -lreadline
endif

.PHONY: all clean install check

all: crepl

# prelude.h is plain C.  Turn it into a sequence of adjacent string literals
# that crepl.c #includes as the initialiser of PRELUDE[].
prelude_inc.h: prelude.h
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $< > $@

crepl: crepl.c prelude_inc.h
	$(CC) $(CFLAGS) $(RL_CFLAGS) $(EXPORT) -o $@ crepl.c $(LIBS) $(RL_LIBS)
	@if [ "$(RLTEST)" != "yes" ]; then \
	  echo "note: libreadline not found -- built without line editing/history"; fi

check: crepl
	@./run-tests.sh

install: crepl
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 crepl $(DESTDIR)$(PREFIX)/bin/crepl

clean:
	rm -f crepl prelude_inc.h .rltest.c .rltest.bin
