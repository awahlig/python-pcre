python-pcre
===========

Python bindings for PCRE regex engine.


Requirements
------------

* PCRE 8.30

So far tested with Python 2.7 only.


Building
--------

TBD


Installation
------------

TBD


Differences between python-pcre and re modules
----------------------------------------------

* slightly different regex syntax
* `sub()`, `subn()`, `expand()` use `str.format()` instead of `\1`
* returned group names are always unicode strings
* `DEBUG`, `LOCALE`, `VERBOSE` flags are not supported
* pattern caching is not supported

Substitution example:

```python
>>> pcre.sub(r'def\s+([a-zA-Z_][a-zA-Z_0-9]*)\s*\(\s*\):',
...          'static PyObject*\npy_{1}(void)\n{{',
...          'def myfunc():')
'static PyObject*\npy_myfunc(void)\n{'
```
Notice the `{1}` and escaped `{{` in repl string.

The built-in re module would use `\1` and a raw string instead:
`r'static PyObject*\npy_\1(void)\n{'`

The arguments used in `str.format()` call are:
* all groups starting from group 0 (entire match) as positional arguments,
* all named groups as keyword arguments.


License
-------

```
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
```
