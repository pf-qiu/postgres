PROGRAM    = reptest
OBJS       = reptest.o libpqwalreceiver.o myelog.o pqformat.o stringinfo.o walproto.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport) -lstdc++
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)