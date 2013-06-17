# suggested by libtoolize
ACLOCAL_AMFLAGS = -I m4
DEFS = @DEFS@ \
	-DDEFAULT_CONFIGDIR=\"$(sysconfdir)\"

man_MANS = pgpool.8

pgpool.8: pgpool.8.in
	sed 's,@sysconfdir\@,$(sysconfdir),g' $? >$@

EXTRA_DIST = pgpool.8.in \
		pgpool-ja.html \
		pgpool-en.html \
		pgpool-zh_cn.html \
		wd-ja.jpg \
		wd-en.jpg \
		tutorial-ja.html \
		tutorial-en.html \
		tutorial-zh_cn.html \
	    pgpool.css \
		pgpool-ja.css \
		where_to_send_queries.pdf \
		where_to_send_queries.odg \
		basebackup.sh \
		pgpool_remote_start \
		recovery.conf.sample

CLEANFILES = pgpool.8

