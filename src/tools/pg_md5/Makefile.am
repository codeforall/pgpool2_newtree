AM_CPPFLAGS = -D_GNU_SOURCE -I $(top_srcdir)/src/include -I @PGSQL_INCLUDE_DIR@
bin_PROGRAMS = pg_md5

pg_md5_SOURCES = pg_md5.c \
				$(top_srcdir)/src/utils/md5.c \
		    	$(top_srcdir)/src/utils/pool_error.c \
			    $(top_srcdir)/src/utils/pool_signal.c \
				$(top_srcdir)/src/utils/strlcpy.c \
	    		$(top_srcdir)/src/main/pool_config_md5.c \
				$(top_srcdir)/src/main/pool_passwd.c \
			    $(top_srcdir)/src/main/pool_globals.c

DEFS = @DEFS@ \
    -DDEFAULT_CONFIGDIR=\"$(sysconfdir)\"

