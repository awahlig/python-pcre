# PCRE - Perl-Compatible Regular Expressions
#
# Differences between pcre and re modules.
# - slightly different regex syntax
# - sub(), subn(), expand() use str.format() instead of \1 subst. (see below)
# - returned group names are always unicode strings
# - DEBUG, LOCALE, VERBOSE flags are not supported
# - pattern caching is not supported
#
# Substitution example:
#   >>> pcre.sub(r'def\s+([a-zA-Z_][a-zA-Z_0-9]*)\s*\(\s*\):',
#   ...          'static PyObject*\npy_{1}(void)\n{{',
#   ...          'def myfunc():')
#   'static PyObject*\npy_myfunc(void)\n{'
#
#   Notice the {1} and escaped {{ in repl string.
#
#   The built-in re module would use \1 and a raw string instead:
#     r'static PyObject*\npy_\1(void)\n{'
#
#   The arguments used in str.format() call are:
#     all groups starting from 0 (entire match) as positional arguments,
#     all named groups as keyword arguments.
#

import _pcre


class Match(_pcre.Match):
    def expand(self, template):
        return template.format(self.group(), *self.groups(), **self.groupdict())


class Pattern(_pcre.Pattern):
    # Tell _pcre.Pattern to use this subclass for match instances.
    _match_class = Match

    def search(self, string, pos=-1, endpos=-1):
        return self(string, pos, endpos)

    def match(self, string, pos=-1, endpos=-1):
        return self(string, pos, endpos, _pcre.ANCHORED)

    def split(self, string, maxsplit=0):
        output = []
        pos = 0
        n = 0
        for match in self.finditer(string):
            output.append(string[pos:match.start()])
            output.extend(match.groups())
            pos = match.end()
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
        if not callable(repl):
            repl = lambda m, s=repl: s.format(m.group(), *m.groups(), **m.groupdict())
        output = []
        pos = 0
        n = 0
        for match in self.finditer(string):
            output.extend((string[pos:match.start()], repl(match)))
            pos = match.end()
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


_alnum = frozenset('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890')
compile = Pattern
error = _pcre.PCREError

I = IGNORECASE = _pcre.IGNORECASE
M = MULTILINE = _pcre.MULTILINE
S = DOTALL = _pcre.DOTALL
U = UNICODE = _pcre.UNICODE

__version__ = _pcre.version()
