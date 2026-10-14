#!/usr/bin/env python3
"""
mailserver.py - a fake IMAP server for manxmail's tests: plain TCP on
127.0.0.1 (port 1143, or the first argument), user "test", password
"secret", an INBOX of a few messages (plain text, HTML with a plain
alternative, Latin-1 quoted-printable, an attachment, encoded-word
headers, a long newsletter) and an empty Sent folder.

It speaks what manxmail uses: CAPABILITY, LOGIN, AUTHENTICATE PLAIN (in
the command or after a "+"), LIST, SELECT/EXAMINE, FETCH and UID FETCH
(UID FLAGS INTERNALDATE RFC822.SIZE ENVELOPE BODYSTRUCTURE BODY[...]
BODY.PEEK[...]), UID STORE, NOOP, LOGOUT. Each command is logged to
stdout (never a password).

    python3 tests/mailserver.py [PORT] [--no-sasl-ir] [--idle=SECS]

--idle: a connection left that long is dropped (with a BYE), as Gmail
drops one after half an hour.
"""
import base64
import email
import email.policy
import re
import socket
import sys
import threading
from email.mime.application import MIMEApplication
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText

USER, PASS = 'test', 'secret'


def messages():
    out = []
    m = MIMEText('Lunch on Thursday? The place is at https://example.com/menu\n'
                 '\nAlice\n', 'plain', 'utf-8')
    m['From'] = 'Alice <alice@example.com>'
    m['To'] = 'test@example.com'
    m['Subject'] = 'Lunch?'
    m['Date'] = 'Mon, 12 Oct 2026 12:30:00 -0600'
    out.append((m, ['\\Seen']))

    m = MIMEMultipart('alternative')
    m['From'] = '=?UTF-8?Q?Ren=C3=A9e?= <renee@example.org>'
    m['To'] = 'test@example.com'
    m['Subject'] = '=?UTF-8?B?R3LDvMOfZSBmcm9tIFrDvHJpY2g=?='
    m['Date'] = 'Tue, 13 Oct 2026 09:15:00 +0200'
    m.attach(MIMEText('Gruesse! The plain version.\n', 'plain', 'utf-8'))
    m.attach(MIMEText('<html><body><h1>Grüße</h1><p>From <b>Zürich</b>,'
                      ' with a <a href="https://example.org/photos">photo album</a>.</p>'
                      '<table border=1><tr><td>One</td><td>Two</td></tr></table>'
                      '</body></html>', 'html', 'utf-8'))
    out.append((m, []))

    m = email.message_from_string(
        'From: Bob <bob@example.net>\nTo: test@example.com\nSubject: Le menu\n'
        'Date: Tue, 13 Oct 2026 18:00:00 +0100\nMIME-Version: 1.0\n'
        'Content-Type: text/plain; charset="iso-8859-1"\n'
        'Content-Transfer-Encoding: quoted-printable\n\n'
        'Caf=E9 au lait, cr=E8me br=FBl=E9e.\n')
    out.append((m, []))

    m = MIMEMultipart('mixed')
    m['From'] = 'Carol <carol@example.com>'
    m['To'] = 'test@example.com'
    m['Subject'] = 'The report'
    m['Date'] = 'Wed, 14 Oct 2026 08:00:00 -0600'
    m.attach(MIMEText('The report is attached.\n', 'plain', 'utf-8'))
    pdf = MIMEApplication(b'%PDF-1.4 fake report\n' * 20, 'pdf')
    pdf.add_header('Content-Disposition', 'attachment', filename='report.pdf')
    m.attach(pdf)
    out.append((m, ['\\Flagged']))

    paras = ''.join('<p>Item %d: the news of the day, with <a href="https://example.com/%d">'
                    'a link</a>.</p>' % (i, i) for i in range(1, 41))
    m = MIMEText('<html><body><h2>Newsletter</h2>' + paras + '</body></html>', 'html', 'utf-8')
    m['From'] = 'News <news@example.com>'
    m['To'] = 'test@example.com'
    m['Subject'] = 'Weekly news'
    m['Date'] = 'Wed, 14 Oct 2026 10:00:00 -0600'
    out.append((m, []))
    return out


def q(s):
    """an IMAP string: quoted, or a literal when it can't be"""
    if s is None:
        return 'NIL'
    if any(ord(c) > 126 or c in '\r\n' for c in s):
        b = s.encode('utf-8')
        return '{%d}\r\n' % len(b) + b.decode('utf-8')
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def addrs(v):
    if not v:
        return 'NIL'
    out = []
    for name, addr in email.utils.getaddresses([v]):
        box, _, host = addr.partition('@')
        out.append('(%s NIL %s %s)' % (q(name) if name else 'NIL', q(box), q(host)))
    return '(' + ''.join(out) + ')'


def envelope(m):
    return '(%s %s %s %s %s %s %s %s %s %s)' % (
        q(m['Date']), q(m['Subject']), addrs(m['From']), addrs(m['From']),
        addrs(m['From']), addrs(m['To']), addrs(m['Cc']), 'NIL', 'NIL',
        q(m['Message-ID']))


def raw_payload(p):
    pl = p.get_payload()
    return pl.encode('utf-8') if isinstance(pl, str) else b''


def bodystructure(p):
    if p.is_multipart():
        return '(%s %s ("boundary" %s) NIL NIL)' % (
            ''.join(bodystructure(c) for c in p.get_payload()),
            q(p.get_content_subtype()), q(p.get_boundary()))
    params = []
    cs = p.get_param('charset')
    if cs:
        params += ['"charset"', q(cs)]
    name = p.get_param('name')
    if name:
        params += ['"name"', q(name)]
    enc = p.get('Content-Transfer-Encoding', '7bit')
    size = len(raw_payload(p))
    disp = 'NIL'
    if p.get_content_disposition() == 'attachment':
        disp = '("attachment" ("filename" %s))' % q(p.get_filename())
    pl = '(' + ' '.join(params) + ')' if params else 'NIL'
    if p.get_content_maintype() == 'text':
        return '(%s %s %s NIL NIL %s %d %d NIL %s NIL)' % (
            q(p.get_content_maintype()), q(p.get_content_subtype()), pl, q(enc), size,
            raw_payload(p).count(b'\n'), disp)
    return '(%s %s %s NIL NIL %s %d NIL %s NIL)' % (
        q(p.get_content_maintype()), q(p.get_content_subtype()), pl, q(enc), size, disp)


def section(m, sec):
    """the raw bytes of BODY[sec]"""
    if sec == '':
        return m.as_bytes()
    if sec == 'HEADER':
        return m.as_bytes().split(b'\n\n', 1)[0] + b'\n\n'
    p = m
    for n in sec.split('.'):
        if not n.isdigit():
            return b''
        if p.is_multipart():
            kids = p.get_payload()
            i = int(n) - 1
            if i >= len(kids):
                return b''
            p = kids[i]
        elif n != '1':
            return b''
    return raw_payload(p)


def seqset(s, top):
    out = []
    for part in s.split(','):
        a, _, b = part.partition(':')
        lo = top if a == '*' else int(a)
        hi = lo if not b else (top if b == '*' else int(b))
        if lo > hi:
            lo, hi = hi, lo
        out += range(lo, hi + 1)
    return out


class Session:
    def __init__(self, c, sasl_ir, idle):
        self.c, self.sasl_ir, self.idle = c, sasl_ir, idle
        self.box = None
        self.mail = {'INBOX': [[m, list(f), i + 1] for i, (m, f) in enumerate(messages())],
                     'Sent': []}
        self.f = c.makefile('rb')

    def send(self, s):
        self.c.sendall(s.encode('utf-8') if isinstance(s, str) else s)

    def caps(self):
        return 'IMAP4rev1 AUTH=PLAIN' + (' SASL-IR' if self.sasl_ir else '')

    def run(self):
        self.send('* OK [CAPABILITY %s] fake IMAP ready\r\n' % self.caps())
        if self.idle:
            self.c.settimeout(self.idle)
        while True:
            try:
                line = self.f.readline()
            except socket.timeout:
                print('-- idle: dropped', flush=True)
                self.c.settimeout(None)
                self.send('* BYE idle too long\r\n')
                self.c.close()
                return
            if not line:
                return
            line = line.decode('utf-8', 'replace').rstrip('\r\n')
            tag, _, rest = line.partition(' ')
            cmd, _, args = rest.partition(' ')
            cmd = cmd.upper()
            uid = False
            if cmd == 'UID':
                uid = True
                cmd, _, args = args.partition(' ')
                cmd = cmd.upper()
            shown = line if cmd not in ('LOGIN', 'AUTHENTICATE') else tag + ' ' + cmd + ' ...'
            print('<<', shown, flush=True)
            if not self.do(tag, cmd, args, uid):
                return

    def ok(self, tag, text='done'):
        self.send('%s OK %s\r\n' % (tag, text))

    def do(self, tag, cmd, args, uid):
        if cmd == 'CAPABILITY':
            self.send('* CAPABILITY %s\r\n' % self.caps())
            self.ok(tag)
        elif cmd == 'LOGIN':
            parts = [x[0].replace('\\"', '"').replace('\\\\', '\\') if x[0] else x[1]
                     for x in re.findall(r'"((?:[^"\\]|\\.)*)"|(\S+)', args)]
            if len(parts) == 2 and parts[0] == USER and parts[1] == PASS:
                self.ok(tag, 'logged in')
            else:
                self.send('%s NO [AUTHENTICATIONFAILED] Invalid credentials\r\n' % tag)
        elif cmd == 'AUTHENTICATE':
            mech, _, b = args.partition(' ')
            if not b:
                self.send('+ \r\n')
                b = self.f.readline().decode().strip()
            try:
                z, u, p = base64.b64decode(b).split(b'\0')
                good = u.decode() == USER and p.decode() == PASS
            except Exception:
                good = False
            if good:
                self.ok(tag, '[CAPABILITY %s] logged in' % self.caps())
            else:
                self.send('%s NO [AUTHENTICATIONFAILED] Invalid credentials\r\n' % tag)
        elif cmd == 'LIST':
            for name in self.mail:
                self.send('* LIST (\\HasNoChildren) "/" %s\r\n' % q(name))
            self.ok(tag)
        elif cmd in ('SELECT', 'EXAMINE'):
            name = args.strip('"')
            if name not in self.mail:
                self.send('%s NO no such mailbox\r\n' % tag)
                return True
            self.box = name
            msgs = self.mail[name]
            self.send('* %d EXISTS\r\n* 0 RECENT\r\n' % len(msgs))
            self.send('* OK [UIDVALIDITY 42] UIDs valid\r\n')
            self.send('* OK [UIDNEXT %d] next\r\n' % (len(msgs) + 1))
            self.ok(tag, '[READ-WRITE] selected')
        elif cmd == 'FETCH':
            self.fetch(tag, args, uid)
        elif cmd == 'STORE':
            s, _, rest = args.partition(' ')
            what, _, flags = rest.partition(' ')
            flags = flags.strip('()').split()
            msgs = self.mail.get(self.box, [])
            for i, msg in enumerate(msgs):
                if (msg[2] if uid else i + 1) in seqset(s, len(msgs)):
                    for fl in flags:
                        if what.upper().startswith('+') and fl not in msg[1]:
                            msg[1].append(fl)
                        elif what.upper().startswith('-') and fl in msg[1]:
                            msg[1].remove(fl)
            self.ok(tag)
        elif cmd == 'NOOP':
            self.ok(tag)
        elif cmd == 'LOGOUT':
            self.send('* BYE\r\n')
            self.ok(tag)
            return False
        else:
            self.send('%s BAD %s not known here\r\n' % (tag, cmd))
        return True

    def fetch(self, tag, args, uid):
        s, _, items = args.partition(' ')
        items = items.strip()
        if items.startswith('(') and items.endswith(')'):
            items = items[1:-1]
        want = re.findall(r'BODY(?:\.PEEK)?\[[^\]]*\]|\S+', items)
        msgs = self.mail.get(self.box, [])
        nums = seqset(s, msgs[-1][2] if (uid and msgs) else len(msgs))
        for i, (m, flags, u) in enumerate(msgs):
            if (u if uid else i + 1) not in nums:
                continue
            out = ['UID %d' % u] if uid else []
            for w in want:
                W = w.upper()
                if W == 'UID' and not uid:
                    out.append('UID %d' % u)
                elif W == 'FLAGS':
                    out.append('FLAGS (%s)' % ' '.join(flags))
                elif W == 'INTERNALDATE':
                    d = email.utils.parsedate_to_datetime(m['Date'])
                    out.append('INTERNALDATE "%s"' % d.strftime('%d-%b-%Y %H:%M:%S %z'))
                elif W == 'RFC822.SIZE':
                    out.append('RFC822.SIZE %d' % len(m.as_bytes()))
                elif W == 'ENVELOPE':
                    out.append('ENVELOPE ' + envelope(m))
                elif W == 'BODYSTRUCTURE':
                    out.append('BODYSTRUCTURE ' + bodystructure(m))
                elif W.startswith('BODY'):
                    sec = w[w.index('[') + 1:-1]
                    data = section(m, sec)
                    out.append('BODY[%s] {%d}\r\n' % (sec, len(data)) + data.decode('latin-1'))
                    if not W.startswith('BODY.PEEK') and '\\Seen' not in flags:
                        flags.append('\\Seen')
            body = '* %d FETCH (%s)\r\n' % (i + 1, ' '.join(out))
            self.send(body.encode('utf-8'))
        self.ok(tag)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    port = int(args[0]) if args else 1143
    sasl_ir = '--no-sasl-ir' not in sys.argv
    idle = 0
    for a in sys.argv[1:]:
        if a.startswith('--idle='):
            idle = float(a[7:])
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', port))
    s.listen(5)
    print('fake IMAP on 127.0.0.1:%d' % port, flush=True)
    while True:
        c, _ = s.accept()
        threading.Thread(target=lambda c=c: Session(c, sasl_ir, idle).run(), daemon=True).start()


if __name__ == '__main__':
    main()
