SUBDIRS = daud src man tests

if INSTALL_UNIT
systemdsystemunit_DATA = systemd/9share.service
endif

# the first-run script and the one that makes the daemon start at boot the way this machine does it
bin_SCRIPTS = tools/9share-setup
pkglibexec_SCRIPTS = tools/9share-init

# the commented template of /etc/9share/9share.conf: put there only if there is no file yet (a package manager keeps its own care of it)
install-data-local:
	$(MKDIR_P) $(DESTDIR)$(sysconfdir)/9share
	test -e $(DESTDIR)$(sysconfdir)/9share/9share.conf || $(INSTALL_DATA) $(srcdir)/etc/9share.conf $(DESTDIR)$(sysconfdir)/9share/9share.conf

EXTRA_DIST = etc/9share.conf debian/9share.init debian/9share.postinst debian/9share.prerm README INSTALL HACKING NEWS ChangeLog AUTHORS COPYING \
             grammars/9share.hbnf tools/regen-conf.sh systemd/9share.service.in \
             docs/index.md docs/_config.yml debian/control debian/rules debian/changelog debian/copyright debian/docs debian/source/format

# the Debian package, built from a clean copy of the source
deb: dist
	rm -rf debbuild && mkdir debbuild && cp $(PACKAGE)-$(VERSION).tar.gz debbuild/$(PACKAGE)_$(VERSION).orig.tar.gz
	cd debbuild && tar -xzf $(PACKAGE)_$(VERSION).orig.tar.gz && cd $(PACKAGE)-$(VERSION) && dpkg-buildpackage -us -uc -b
	@ls debbuild/*.deb

.PHONY: deb
