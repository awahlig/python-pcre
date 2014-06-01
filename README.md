python-pcre
===========

Python bindings for PCRE regex engine.


Requirements
------------

* PCRE (http://www.pcre.org)
* Python (http://python.org/)

So far tested with PCRE 8.30 and Python 2.7 only.


Building and installation
-------------------------

A standard distutils `setup.py` script is provided.
After making sure all dependencies are installed, building
and installation should be as simple as:

```
$ python setup.py build install
```


Differences between python-pcre and re
--------------------------------------

The API is very similar to that of the built-in `re` module:
* http://docs.python.org/library/re.html

Differences:

* slightly different regex syntax
* `sub()`, `subn()`, `expand()` use `str.format()` instead of `\1` substitution
* `DEBUG` and `LOCALE` flags are not supported
* pattern caching is not supported
* only str and unicode are supported as input
* scanner APIs are not supported

For a comprehensive PCRE regex syntax you can visit PHP documentation:
* http://php.net/manual/en/reference.pcre.pattern.syntax.php


Substitution
------------

python-pcre uses `str.format()` substitution instead of the `re`-style
`\1` and `\g<name>`.

Example:

```python
>>> pcre.sub(r'def\s+([a-zA-Z_][a-zA-Z_0-9]*)\s*\(\s*\):',
...          'static PyObject*\npy_{1}(void)\n{{',
...          'def myfunc():')
'static PyObject*\npy_myfunc(void)\n{'
```
Notice the `{1}` and escaped `{{` in repl string.

The built-in re module would use `\\1` instead:
`'static PyObject*\npy_\\1(void)\n{'`

The arguments used in `str.format()` call are:
* all groups starting from group 0 (entire match) as positional arguments,
* all named groups as keyword arguments.

For backward compatibility a conversion function is provided.

Example:

```python
>>> pcre.convert_template('static PyObject*\npy_\\1(void)\n{')
'static PyObject*\npy_{1}(void)\n{{'
```


Unicode handling
----------------

python-pcre internally uses the 8-bit interface of the PCRE library.
The library can operate either on simple 8-bit characters or in UTF-8 mode.

The mode is selected when a pattern is compiled by adding `pcre.UTF8` to
the flags argument.  When in UTF-8 mode, the PCRE library expects both pattern
and the subject string to be valid UTF-8 strings.  This is what python-pcre
requires when Python (binary) string objects are used.

python-pcre also allows unicode strings to be specified and it internally
converts them to UTF-8 strings before calling PCRE APIs.  If a unicode string
is specified when compiling a pattern, the UTF-8 flag is enabled automatically.

When matching, the start/end offsets and offsets returned by `start()`,
`end()` and `span()` are always indexes of the specified subject string.
python-pcre takes care of any needed fixups resulting from internal UTF-8
conversions.


Why
---

I created python-pcre because I was unsatisfied with the performance of
regular expression parser in `re`.  PCRE has a fast parser written in C
whereas `re`'s parser is written in pure Python (only the matching is done
in C).  As a result, Python libraries building and compiling many regular
expressions at import time have long load-times on slower (embedded) systems.

Most of the time such libraries can be sped up by simply replacing
`import re` with `import pcre as re` (unless substitution is used).


License
-------

```
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
```
