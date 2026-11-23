CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
PREFIX  ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin
UNITDIR ?= /etc/systemd/system
MODDIR  ?= /etc/modules-load.d
DESTDIR ?=

all: nbm-touchd

nbm-touchd: src/nbm-touchd.c
	$(CC) $(CFLAGS) -o $@ $<

install: nbm-touchd
	install -d $(DESTDIR)$(SBINDIR)
	install -m 755 nbm-touchd $(DESTDIR)$(SBINDIR)/nbm-touchd
	install -d $(DESTDIR)$(UNITDIR)
	install -m 644 systemd/nbm-touchd.service $(DESTDIR)$(UNITDIR)/nbm-touchd.service
	install -d $(DESTDIR)$(MODDIR)
	install -m 644 systemd/nbm-touchd.modules $(DESTDIR)$(MODDIR)/nbm-touchd.conf
	@echo
	@echo "Installed. Enable and start with:"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable --now nbm-touchd"

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/nbm-touchd
	rm -f $(DESTDIR)$(UNITDIR)/nbm-touchd.service
	rm -f $(DESTDIR)$(MODDIR)/nbm-touchd.conf
	@echo "Uninstalled. Run 'systemctl daemon-reload' to finish."

clean:
	rm -f nbm-touchd

.PHONY: all install uninstall clean
