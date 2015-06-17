""" python-pcre

Copyright (c) 2012-2015, Arkadiusz Wahlig
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

__version__ = '0.6'

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
        return (string[:0].join(output), n)

    def __reduce__(self):
        if self.pattern is None:
            return (Pattern, (None, 0, self.dumps()))
        return (Pattern, (self.pattern, self.flags))

    def __repr__(self):
        if self.pattern is None:
            return '{0}.loads({1})'.format(__name__, repr(self.dumps()))
        flags = self.flags
        if flags:
            v = []
            for name in _FLAGS:
                value = getattr(_pcre, name)
                if flags & value:
                    v.append('{0}.{1}'.format(__name__, name))
                    flags &= ~value
            if flags:
                v.append(hex(flags))
            return '{0}.compile({1}, {2})'.format(__name__, repr(self.pattern), '|'.join(v))
        return '{0}.compile({1})'.format(__name__, repr(self.pattern))

class Match(_pcre.Match):
    def expand(self, template):
        return template.format(self.group(), *self.groups(''), **self.groupdict(''))

    def __repr__(self):
        cls = self.__class__
        return '<{0}.{1} object; span={2}, match={3}>'.format(cls.__module__,
            cls.__name__, repr(self.span()), repr(self.group()))

class REMatch(Match):
    def expand(self, template):
        groups = (self.group(),) + self.groups()
        groupdict = self.groupdict()
        def repl(match):
            esc, index, group, badgroup = match.groups()
            if esc:
                return ('\\' + esc).decode('string-escape')
            if badgroup:
                raise PCREError(100, 'invalid group name')
            try:
                if index or group.isdigit():
                    result = groups[int(index or group)]
                else:
                    result = groupdict[group]
            except IndexError:
                raise PCREError(15, 'invalid group reference')
            except KeyError:
                raise IndexError('unknown group name')
            if result is None:
                raise PCREError(101, 'unmatched group')
            return result
        return _REGEX_RE_TEMPLATE.sub(repl, template)

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

def loads(data):
    # Loads a pattern serialized with Pattern.dumps().
    return Pattern(None, loads=data)

def escape(pattern):
    # Escapes a regular expression.
    s = list(pattern)
    alnum = _ALNUM
    for i, c in enumerate(pattern):
        if c not in alnum:
            s[i] = '\\000' if c == '\000' else ('\\' + c)
    return pattern[:0].join(s)

def escape_template(template):
    # Escapes "{" and "}" characters in the template.
    return template.replace('{', '{{').replace('}', '}}')

def convert_re_template(template):
    # Converts re template r"\1\g<id>" to "{1}{id}" format.
    def repl(match):
        esc, index, group, badgroup = match.groups()
        if esc:
            return ('\\' + esc).decode('string-escape')
        if badgroup:
            raise PCREError(100, 'invalid group name')
        return '{%s}' % (index or group)
    return _REGEX_RE_TEMPLATE.sub(repl, escape_template(template))

def enable_re_template_mode():
    # Makes calls to sub() take re templates instead of str.format() templates.
    global Match
    Match = REMatch

_ALNUM = frozenset('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890')
error = PCREError = _pcre.PCREError
NoMatch = _pcre.NoMatch
MAXREPEAT = 65536

# Provides PCRE build-time configuration.
config = type('config', (), _pcre.get_config())

# Pattern and/or match flags
_FLAGS = ('IGNORECASE', 'MULTILINE', 'DOTALL', 'UNICODE', 'VERBOSE',
          'ANCHORED', 'NOTBOL', 'NOTEOL', 'NOTEMPTY', 'NOTEMPTY_ATSTART',
          'UTF8', 'NO_UTF8_CHECK')

# Copy flags from _pcre module
ns = globals()
for name in _FLAGS:
    ns[name] = getattr(_pcre, name)
del ns, name

# Short versions
I = IGNORECASE
M = MULTILINE
S = DOTALL
U = UNICODE
X = VERBOSE

# Study flags
STUDY_JIT = _pcre.STUDY_JIT

# Used to parse re templates.
_REGEX_RE_TEMPLATE = compile(r'\\(?:([\\abfnrtv]|0[0-7]{0,2}|[0-7]{3})|'
                             r'(\d{1,2})|g<(\d+|[^\d\W]\w*)>|(g[^>]*))')
