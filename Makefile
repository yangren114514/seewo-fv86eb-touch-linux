CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
PREFIX  ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin
UNITDIR ?= /etc/systemd/system
MODDIR  ?= /etc/modules-load.d
DESTDIR ?=

all: seewo-touchd

seewo-touchd: src/seewo-touchd.c
	$(CC) $(CFLAGS) -o $@ $<

install: seewo-touchd
	install -d $(DESTDIR)$(SBINDIR)
	install -m 755 seewo-touchd $(DESTDIR)$(SBINDIR)/seewo-touchd
	install -d $(DESTDIR)$(UNITDIR)
	install -m 644 systemd/seewo-touchd.service $(DESTDIR)$(UNITDIR)/seewo-touchd.service
	install -d $(DESTDIR)$(MODDIR)
	install -m 644 systemd/seewo-touchd.modules $(DESTDIR)$(MODDIR)/seewo-touchd.conf
	@echo
	@echo "Installed. Enable and start with:"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable --now seewo-touchd"

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/seewo-touchd
	rm -f $(DESTDIR)$(UNITDIR)/seewo-touchd.service
	rm -f $(DESTDIR)$(MODDIR)/seewo-touchd.conf
	@echo "Uninstalled. Run 'systemctl daemon-reload' to finish."

clean:
	rm -f seewo-touchd

.PHONY: all install uninstall clean
