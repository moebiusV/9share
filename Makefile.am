SUBDIRS = daud src man tests

if INSTALL_UNIT
systemdsystemunit_DATA = systemd/p9share.service
endif

EXTRA_DIST = README INSTALL HACKING NEWS ChangeLog AUTHORS COPYING \
             grammars/p9share.hbnf tools/regen-conf.sh systemd/p9share.service.in \
             docs/index.md docs/_config.yml debian/control debian/rules debian/changelog debian/copyright debian/docs debian/source/format

# the Debian package, built from a clean copy of the source
deb: dist
	rm -rf debbuild && mkdir debbuild && cp $(PACKAGE)-$(VERSION).tar.gz debbuild/$(PACKAGE)_$(VERSION).orig.tar.gz
	cd debbuild && tar -xzf $(PACKAGE)_$(VERSION).orig.tar.gz && cd $(PACKAGE)-$(VERSION) && dpkg-buildpackage -us -uc -b
	@ls debbuild/*.deb

.PHONY: deb
