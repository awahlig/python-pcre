""" python-pcre

Copyright (c) 2012, Arkadiusz Wahlig
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"""

import _pcre


class Match(_pcre.Match):
    def expand(self, template):
        return template.format(self.group(), *self.groups(), **self.groupdict())


class Pattern(_pcre.Pattern):
    # Tell _pcre.Pattern to use this subtype for match instances.
    _match_type = Match

    def search(self, string, pos=-1, endpos=-1):
        return self(string, pos, endpos)

    def match(self, string, pos=-1, endpos=-1):
        return self(string, pos, endpos, _pcre.ANCHORED)

    def split(self, string, maxsplit=0):
        output = []
        pos = 0
        n = 0
        for match in self.finditer(string):
            start, end = match.span()
            output.append(string[pos:start])
            output.extend(match.groups())
            pos = end
            n += 1
            if 0 < maxsplit <= n:
                break
        output.append(string[pos:])
        return output

    def findall(self, string, pos=-1, endpos=-1):
        return [m.group() for m in self.finditer(string, pos, endpos)]

    def finditer(self, string, pos=-1, endpos=-1):
        while 1:
            match = self(string, pos, endpos)
            if match is None:
                break
            yield match
            pos = match.end()

    def sub(self, repl, string, count=0):
        return self.subn(repl, string, count)[0]

    def subn(self, repl, string, count=0):
        if not hasattr(repl, '__call__'):
            repl = lambda match, tmpl=repl: match.expand(tmpl)
        output = []
        pos = 0
        n = 0
        for match in self.finditer(string):
            start, end = match.span()
            output.extend((string[pos:start], repl(match)))
            pos = end
            n += 1
            if 0 < count <= n:
                break
        output.append(string[pos:])
        return string[:0].join(output), n


def match(pattern, string, flags=0):
    return Pattern(pattern, flags).match(string)

def search(pattern, string, flags=0):
    return Pattern(pattern, flags).search(string)

def split(pattern, string, maxsplit=0, flags=0):
    return Pattern(pattern, flags).split(string, maxsplit)

def findall(pattern, string, flags=0):
    return Pattern(pattern, flags).findall(string)

def finditer(pattern, string, flags=0):
    return Pattern(pattern, flags).finditer(string)

def sub(pattern, repl, string, count=0, flags=0):
    return Pattern(pattern, flags).sub(repl, string, count)

def subn(pattern, repl, string, count=0, flags=0):
    return Pattern(pattern, flags).subn(repl, string, count)

def escape(pattern):
    s = list(pattern)
    alnum = _alnum
    for i, c in enumerate(pattern):
        if c not in alnum:
            s[i] = '\\000' if c == '\000' else ('\\' + c)
    return pattern[:0].join(s)

def convert_template(template, esc='\\', fmt='{%s}'):
    # Converts tempates from "\1\g<id>" to "{1}{id}" format.
    o = []
    append = o.append
    for x in template.split(esc):
        if x[:1].isdigit():
            append(fmt % x[0])
            x = x[1:]
        elif x[:2] == 'g<':
            t, x = x.split('>', 1)
            append(fmt % t[2:])
        elif o:
            append(esc)
        append(x)
    return ''.join(o)


_alnum = frozenset('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890')
compile = Pattern
loads = Pattern.loads
error = PCREError = _pcre.PCREError

# Pattern flags
I = IGNORECASE = _pcre.IGNORECASE
M = MULTILINE = _pcre.MULTILINE
S = DOTALL = _pcre.DOTALL
U = UNICODE = _pcre.UNICODE
UTF8 = _pcre.UTF8

# Study flags
JIT = _pcre.JIT

__version__ = _pcre.version()
