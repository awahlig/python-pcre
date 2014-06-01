""" python-pcre

Copyright (c) 2012-2014, Arkadiusz Wahlig
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

class Pattern(_pcre.Pattern):
    def search(self, string, pos=-1, endpos=-1, flags=0):
        try:
            return Match(self, string, pos, endpos, flags)
        except NoMatch:
            pass

    def match(self, string, pos=-1, endpos=-1, flags=0):
        try:
            return Match(self, string, pos, endpos, flags | ANCHORED)
        except NoMatch:
            pass

    def split(self, string, maxsplit=0, flags=0):
        output = []
        pos = n = 0
        for match in self.finditer(string, flags=flags):
            start, end = match.span()
            if start != end:
                output.append(string[pos:start])
                output.extend(match.groups())
                pos = end
                n += 1
                if 0 < maxsplit <= n:
                    break
        output.append(string[pos:])
        return output

    def findall(self, string, pos=-1, endpos=-1, flags=0):
        matches = self.finditer(string, pos, endpos, flags)
        if self.groups == 0:
            return [m.group() for m in matches]
        if self.groups == 1:
            return [m.groups('')[0] for m in matches]
        return [m.groups('') for m in matches]

    def finditer(self, string, pos=-1, endpos=-1, flags=0):
        try:
            while 1:
                match = Match(self, string, pos, endpos, flags)
                yield match
                start, pos = match.span()
                if pos == start:
                    pos += 1
        except NoMatch:
            pass

    def sub(self, repl, string, count=0, flags=0):
        return self.subn(repl, string, count, flags)[0]

    def subn(self, repl, string, count=0, flags=0):
        if not hasattr(repl, '__call__'):
            repl = lambda match, tmpl=repl: match.expand(tmpl)
        output = []
        pos = n = 0
        for match in self.finditer(string, flags=flags):
            start, end = match.span()
            if not pos == start == end or pos == 0:
                output.extend((string[pos:start], repl(match)))
                pos = end
                n += 1
                if 0 < count <= n:
                    break
        output.append(string[pos:])
        return string[:0].join(output), n

    def __reduce__(self):
        return (Pattern, (self.pattern, self.flags))

class Match(_pcre.Match):
    def expand(self, template):
        return template.format(self.group(), *self.groups(), **self.groupdict())

def compile(pattern, flags=0):
    if isinstance(pattern, _pcre.Pattern):
        if flags != 0:
            raise ValueError('cannot process flags argument with a compiled pattern')
        return pattern
    return Pattern(pattern, flags)

def match(pattern, string, flags=0):
    return compile(pattern, flags).match(string)

def search(pattern, string, flags=0):
    return compile(pattern, flags).search(string)

def split(pattern, string, maxsplit=0, flags=0):
    return compile(pattern, flags).split(string, maxsplit)

def findall(pattern, string, flags=0):
    return compile(pattern, flags).findall(string)

def finditer(pattern, string, flags=0):
    return compile(pattern, flags).finditer(string)

def sub(pattern, repl, string, count=0, flags=0):
    return compile(pattern, flags).sub(repl, string, count)

def subn(pattern, repl, string, count=0, flags=0):
    return compile(pattern, flags).subn(repl, string, count)

def loads(data, pattern=None):
    # Loads a pattern serialized with Pattern.dumps().
    return Pattern(pattern, loads=data)

def escape(pattern):
    # Escapes a regular expression.
    s = list(pattern)
    alnum = _alnum
    for i, c in enumerate(pattern):
        if c not in alnum:
            s[i] = '\\000' if c == '\000' else ('\\' + c)
    return pattern[:0].join(s)

def escape_template(template):
    # Escapes "{" and "}" characters in the template.
    return template.replace('{', '{{').replace('}', '}}')

def convert_template(template):
    # Converts re template "\1\g<id>" to "{1}{id}" format.
    repl = lambda m: '{%s}' % (m.group(1) or m.group(2))
    return sub(r'\\(\d+)|\\g<(\w+)>', repl, escape_template(template))

_alnum = frozenset('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890')
error = PCREError = _pcre.PCREError
NoMatch = _pcre.NoMatch

# Pattern and/or match flags
I = IGNORECASE = _pcre.IGNORECASE
M = MULTILINE = _pcre.MULTILINE
S = DOTALL = _pcre.DOTALL
U = UNICODE = _pcre.UNICODE
X = VERBOSE = _pcre.VERBOSE
ANCHORED = _pcre.ANCHORED
UTF8 = _pcre.UTF8
NO_UTF8_CHECK = _pcre.NO_UTF8_CHECK

# Study flags
JIT = _pcre.JIT

__version__ = '0.2'
__pcre_version__ = _pcre.version()
