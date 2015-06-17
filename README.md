python-pcre
===========

Python bindings for PCRE regex engine.


Requirements
------------

* [PCRE](http://www.pcre.org) 8.x
* [Python](http://python.org) 2.6+ or 3.x

Tested with Python 2.6, 2.7, 3.4 and PCRE 8.12, 8.30, 8.35.


Building and installation
-------------------------

A standard distutils `setup.py` script is provided.
After making sure all dependencies are installed, building
and installation should be as simple as:

```
$ python setup.py build install
```

When building PCRE, UTF-8 mode must be enabled (`./configure --enable-utf`).  You might
also want to enable stackless recursion (`--disable-stack-for-recursion`) and unicode
character properties (`--enable-unicode-properties`).  If you plan to use JIT,
add `--enable-jit`.


Differences between python-pcre and re
--------------------------------------

The API is very similar to that of the built-in
[re module](http://docs.python.org/library/re.html).

Differences:

* slightly different regex syntax
* by default, `sub()`, `subn()`, `expand()` use `str.format()` instead of `\1` substitution
  (see below)
* `DEBUG` and `LOCALE` flags are not supported
* patterns are not cached
* scanner APIs are not supported

For a comprehensive PCRE regex syntax you can visit
[PHP documentation](http://php.net/manual/en/reference.pcre.pattern.syntax.php).


Substitution
------------

By default, python-pcre uses `str.format()` instead of the `re`-style `\1` and `\g<name>`
substitution in calls to `sub()`, `subn()` and `expand()`.

Example:

```python
>>> import pcre
>>> pcre.sub(r'def\s+([a-zA-Z_][a-zA-Z_0-9]*)\s*\(\s*\):',
...          'static PyObject*\npy_{1}(void)\n{{',  # str.format() template
...          'def myfunc():')
'static PyObject*\npy_myfunc(void)\n{'
```
Note `{1}` and escaped `{{` in the template string.

The built-in re module would use `\1` instead:
`r'static PyObject*\npy_\1(void)\n{'`

Named groups are referenced using `{name}` instead of `\g<name>`.

Entire match can be referenced using `{0}`.

This makes the template string easier to read and means that it no longer needs to be
a raw string.

However, `re` template mode can be enabled using `enable_re_template_mode()`.
This might be useful if python-pcre is to be used with existing `re`-based code.

```python
>>> pcre.enable_re_template_mode()
>>> pcre.sub(r'(.)', r'[\1]', 'foo')
'[f][o][o]'
```

A function to convert `re` templates is also provided for those one-off cases.

```python
>>> pcre.convert_re_template(r'static PyObject*\npy_\1(void)\n{')
'static PyObject*\npy_{1}(void)\n{{'
```

A small difference between the two modes is that in `str.format()` mode, groups that
didn't match are replaced with `''` whereas in `re` mode it's an error to reference
such groups in the template.

Also note that in Python 3.x `bytes.format()` is not available so the template needs
to be a `str`.


Unicode handling
----------------

python-pcre internally uses the UTF-8 interface of the PCRE library.

Patterns or matched subjects specified as byte strings that contain ascii characters
only (0-127) are passed to PCRE directly, as ascii is a subset of UTF-8.
Other byte strings are internally re-encoded using a simple Latin1 to UTF-8 codec
which maps characters 128-255 to unicode codepoints of the same value.
This conversion is transparent to the caller.

If you know that your byte strings are UTF-8, you can use the `pcre.UTF8` flag
to tell python-pcre to pass them directly to PCRE.  This flag has to be specified
every time a UTF-8 pattern is compiled or a UTF-8 subject is matched.  Note that
in this mode things like `.` may match multiple bytes:

```python
>>> pcre.compile('.').match(b'\xc3\x9c', flags=pcre.UTF8).group()
b'\xc3\x9c'  # two bytes
>>> _.decode('utf-8')
u'\xdc'  # one character
```

python-pcre also accepts unicode strings as input.  In Python 3.3 or newer, which
implement [PEP 393](http://legacy.python.org/dev/peps/pep-0393/), unicode strings
stored internally as ascii are passed to PCRE directly.  Other internal formats are
encoded into UTF-8 using Python APIs (which use the UTF-8 form cached in the unicode
object if available).  In older Python versions these optimizations are not supported
so all unicode objects require the extra encoding step.

python-pcre also accepts objects supporting the buffer interface, such as `array.array`
objects.  Supported are both old and new buffer APIs with buffers containing either bytes
or unicode characters, with the same UTF-8 encoding strategy as byte/unicode strings.

When internally encoding subject strings to UTF-8, any offsets accepted as input
or provided as output are also converted between byte and character offsets so that
the caller doesn't need to be aware of the conversion -- the offsets are always
indexes into the specified subject string, whether it's a byte string or a unicode
string.


License
-------

```
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
```
