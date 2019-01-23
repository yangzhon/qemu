#
# Mini-Kconfig parser
#
# Copyright (c) 2015 Red Hat Inc.
#
# Authors:
#  Paolo Bonzini <pbonzini@redhat.com>
#
# This work is licensed under the terms of the GNU GPL, version 2
# or, at your option, any later version.  See the COPYING file in
# the top-level directory.

import os
import sys

__all__ = [ 'KconfigParserError', 'KconfigData', 'KconfigParser' ]

# -------------------------------------------
# KconfigData implements the Kconfig semantics.  For now it can only
# detect undefined symbols, i.e. symbols that were referenced in
# assignments or dependencies but were not declared with "config FOO".
#
# Semantic actions are represented by methods called do_*.  The do_var
# method return the semantic value of a variable (which right now is
# just its name).
# -------------------------------------------

class KconfigData:
    def __init__(self):
        self.previously_included = []
        self.incl_info = None
        self.defined_vars = set()
        self.referenced_vars = set()

    # semantic analysis -------------

    def check_undefined(self):
        undef = False
        for i in self.referenced_vars:
            if not (i in self.defined_vars):
                print "undefined symbol %s" % (i)
                undef = True
        return undef

    # semantic actions -------------

    def do_declaration(self, var):
        if (var in self.defined_vars):
            raise Exception('variable "' + var + '" defined twice')

        self.defined_vars.add(var)

    # var is a string with the variable's name.
    #
    # For now this just returns the variable's name itself.
    def do_var(self, var):
        self.referenced_vars.add(var)
        return var

    def do_assignment(self, var, val):
        pass

    def do_default(self, var, val, cond=None):
        pass

    def do_depends_on(self, var, expr):
        pass

    def do_select(self, var, symbol, cond=None):
        pass

# -------------------------------------------
# KconfigParser implements a recursive descent parser for (simplified)
# Kconfig syntax.
# -------------------------------------------

# tokens table
TOKENS = {}
TOK_NONE = -1
TOK_LPAREN = 0;   TOKENS[TOK_LPAREN] = '"("';
TOK_RPAREN = 1;   TOKENS[TOK_RPAREN] = '")"';
TOK_EQUAL = 2;    TOKENS[TOK_EQUAL] = '"="';
TOK_AND = 3;      TOKENS[TOK_AND] = '"&&"';
TOK_OR = 4;       TOKENS[TOK_OR] = '"||"';
TOK_NOT = 5;      TOKENS[TOK_NOT] = '"!"';
TOK_DEPENDS = 6;  TOKENS[TOK_DEPENDS] = '"depends"';
TOK_ON = 7;       TOKENS[TOK_ON] = '"on"';
TOK_SELECT = 8;   TOKENS[TOK_SELECT] = '"select"';
TOK_CONFIG = 9;   TOKENS[TOK_CONFIG] = '"config"';
TOK_DEFAULT = 10; TOKENS[TOK_DEFAULT] = '"default"';
TOK_Y = 11;       TOKENS[TOK_Y] = '"y"';
TOK_N = 12;       TOKENS[TOK_N] = '"n"';
TOK_SOURCE = 13;  TOKENS[TOK_SOURCE] = '"source"';
TOK_BOOL = 14;    TOKENS[TOK_BOOL] = '"bool"';
TOK_IF = 15;      TOKENS[TOK_IF] = '"if"';
TOK_ID = 16;      TOKENS[TOK_ID] = 'identifier';
TOK_EOF = 17;     TOKENS[TOK_EOF] = 'end of file';

class KconfigParserError(Exception):
    def __init__(self, parser, msg, tok=None):
        self.loc = parser.location()
        tok = tok or parser.tok
        if tok != TOK_NONE:
            msg = '%s before %s' %(msg, TOKENS[tok])
        self.msg = msg

    def __str__(self):
        return "%s: %s" % (self.loc, self.msg)

class KconfigParser:
    @classmethod
    def parse(self, fp):
        data = KconfigData()
        parser = KconfigParser(data)
        parser.parse_file(fp)
        if data.check_undefined():
            raise KconfigParserError(parser, "there were undefined symbols")

        return data

    def __init__(self, data):
        self.data = data

    def parse_file(self, fp):
        self.abs_fname = os.path.abspath(fp.name)
        self.fname = fp.name
        self.data.previously_included.append(self.abs_fname)
        self.src = fp.read()
        if self.src == '' or self.src[-1] != '\n':
            self.src += '\n'
        self.cursor = 0
        self.line = 1
        self.line_pos = 0
        self.get_token()
        self.parse_config()

    # file management -----

    def error_path(self):
        inf = self.data.incl_info
        res = ""
        while inf:
            res = ("In file included from %s:%d:\n" % (inf['file'],
                                                       inf['line'])) + res
            inf = inf['parent']
        return res

    def location(self):
        col = 1
        for ch in self.src[self.line_pos:self.pos]:
            if ch == '\t':
                col += 8 - ((col - 1) % 8)
            else:
                col += 1
        return '%s%s:%d:%d' %(self.error_path(), self.fname, self.line, col)

    def do_include(self, include):
        incl_abs_fname = os.path.join(os.path.dirname(self.abs_fname),
                                      include)
        # catch inclusion cycle
        inf = self.data.incl_info
        while inf:
            if incl_abs_fname == os.path.abspath(inf['file']):
                raise KconfigParserError(self, "Inclusion loop for %s"
                                    % include)
            inf = inf['parent']

        # skip multiple include of the same file
        if incl_abs_fname in self.data.previously_included:
            return
        try:
            fp = open(incl_abs_fname, 'r')
        except IOError, e:
            raise KconfigParserError(self,
                                '%s: %s' % (e.strerror, include))

        inf = self.data.incl_info
        self.data.incl_info = { 'file': self.fname, 'line': self.line,
                'parent': inf }
        KconfigParser(self.data).parse_file(fp)
        self.data.incl_info = inf

    # recursive descent parser -----

    # y_or_n: Y | N
    def parse_y_or_n(self):
        if self.tok == TOK_Y:
            self.get_token()
            return True
        if self.tok == TOK_N:
            self.get_token()
            return False
        raise KconfigParserError(self, 'Expected "y" or "n"')

    # var: ID
    def parse_var(self):
        if self.tok == TOK_ID:
            val = self.val
            self.get_token()
            return self.data.do_var(val)
        else:
            raise KconfigParserError(self, 'Expected identifier')

    # assignment_var: ID (starting with "CONFIG_")
    def parse_assignment_var(self):
        if self.tok == TOK_ID:
            val = self.val
            if not val.startswith("CONFIG_"):
                raise KconfigParserError(self,
                           'Expected identifier starting with "CONFIG_"', TOK_NONE)
            self.get_token()
            return self.data.do_var(val[7:])
        else:
            raise KconfigParserError(self, 'Expected identifier')

    # assignment: var EQUAL y_or_n
    def parse_assignment(self):
        var = self.parse_assignment_var()
        if self.tok != TOK_EQUAL:
            raise KconfigParserError(self, 'Expected "="')
        self.get_token()
        self.data.do_assignment(var, self.parse_y_or_n())

    # primary: NOT primary
    #       | LPAREN expr RPAREN
    #       | var
    def parse_primary(self):
        if self.tok == TOK_NOT:
            self.get_token()
            self.parse_primary()
        elif self.tok == TOK_LPAREN:
            self.get_token()
            self.parse_expr()
            if self.tok != TOK_RPAREN:
                raise KconfigParserError(self, 'Expected ")"')
            self.get_token()
        elif self.tok == TOK_ID:
            self.parse_var()
        else:
            raise KconfigParserError(self, 'Expected "!" or "(" or identifier')

    # disj: primary (OR primary)*
    def parse_disj(self):
        self.parse_primary()
        while self.tok == TOK_OR:
            self.get_token()
            self.parse_primary()

    # expr: disj (AND disj)*
    def parse_expr(self):
        self.parse_disj()
        while self.tok == TOK_AND:
            self.get_token()
            self.parse_disj()

    # condition: IF expr
    #       | empty
    def parse_condition(self):
        if self.tok == TOK_IF:
            self.get_token()
            return self.parse_expr()
        else:
            return None

    # property: DEFAULT y_or_n condition
    #       | DEPENDS ON expr
    #       | SELECT var condition
    #       | BOOL
    def parse_property(self, var):
        if self.tok == TOK_DEFAULT:
            self.get_token()
            val = self.parse_y_or_n()
            cond = self.parse_condition()
            self.data.do_default(var, val, cond)
        elif self.tok == TOK_DEPENDS:
            self.get_token()
            if self.tok != TOK_ON:
                raise KconfigParserError(self, 'Expected "on"')
            self.get_token()
            self.data.do_depends_on(var, self.parse_expr())
        elif self.tok == TOK_SELECT:
            self.get_token()
            symbol = self.parse_var()
            cond = self.parse_condition()
            self.data.do_select(var, symbol, cond)
        elif self.tok == TOK_BOOL:
            self.get_token()
        else:
            raise KconfigParserError(self, 'Error in recursive descent?')

    # properties: properties property
    #       | /* empty */
    def parse_properties(self, var):
        had_default = False
        while self.tok == TOK_DEFAULT or self.tok == TOK_DEPENDS or \
              self.tok == TOK_SELECT or self.tok == TOK_BOOL:
            self.parse_property(var)
        self.data.do_default(var, False)

        # for nicer error message
        if self.tok != TOK_SOURCE and self.tok != TOK_CONFIG and \
           self.tok != TOK_ID and self.tok != TOK_EOF:
            raise KconfigParserError(self, 'expected "source", "config", identifier, '
                    + '"default", "depends on" or "select"')

    # declaration: config var properties
    def parse_declaration(self):
        if self.tok == TOK_CONFIG:
            self.get_token()
            var = self.parse_var()
            self.data.do_declaration(var)
            self.parse_properties(var)
        else:
            raise KconfigParserError(self, 'Error in recursive descent?')

    # clause: SOURCE
    #       | declaration
    #       | assignment
    def parse_clause(self):
        if self.tok == TOK_SOURCE:
            val = self.val
            self.get_token()
            self.do_include(val)
        elif self.tok == TOK_CONFIG:
            self.parse_declaration()
        elif self.tok == TOK_ID:
            self.parse_assignment()
        else:
            raise KconfigParserError(self, 'expected "source", "config" or identifier')

    # config: clause+ EOF
    def parse_config(self):
        while self.tok != TOK_EOF:
            self.parse_clause()
        return self.data

    # scanner -----

    def get_token(self):
        while True:
            self.tok = self.src[self.cursor]
            self.pos = self.cursor
            self.cursor += 1

            self.val = None
            self.tok = self.scan_token()
            if self.tok is not None:
                return

    def check_keyword(self, rest):
        if not self.src.startswith(rest, self.cursor):
            return False
        length = len(rest)
        if self.src[self.cursor + length].isalnum() or self.src[self.cursor + length] == '|':
            return False
        self.cursor += length
        return True

    def scan_token(self):
        if self.tok == '#':
            self.cursor = self.src.find('\n', self.cursor)
            return None
        elif self.tok == '=':
            return TOK_EQUAL
        elif self.tok == '(':
            return TOK_LPAREN
        elif self.tok == ')':
            return TOK_RPAREN
        elif self.tok == '&' and self.src[self.pos+1] == '&':
            self.cursor += 1
            return TOK_AND
        elif self.tok == '|' and self.src[self.pos+1] == '|':
            self.cursor += 1
            return TOK_OR
        elif self.tok == '!':
            return TOK_NOT
        elif self.tok == 'd' and self.check_keyword("epends"):
            return TOK_DEPENDS
        elif self.tok == 'o' and self.check_keyword("n"):
            return TOK_ON
        elif self.tok == 's' and self.check_keyword("elect"):
            return TOK_SELECT
        elif self.tok == 'c' and self.check_keyword("onfig"):
            return TOK_CONFIG
        elif self.tok == 'd' and self.check_keyword("efault"):
            return TOK_DEFAULT
        elif self.tok == 'b' and self.check_keyword("ool"):
            return TOK_BOOL
        elif self.tok == 'i' and self.check_keyword("f"):
            return TOK_IF
        elif self.tok == 'y' and self.check_keyword(""):
            return TOK_Y
        elif self.tok == 'n' and self.check_keyword(""):
            return TOK_N
        elif (self.tok == 's' and self.check_keyword("ource")) or \
              self.tok == 'i' and self.check_keyword("nclude"):
            # source FILENAME
            # include FILENAME
            while self.src[self.cursor].isspace():
                self.cursor += 1
            start = self.cursor
            self.cursor = self.src.find('\n', self.cursor)
            self.val = self.src[start:self.cursor]
            return TOK_SOURCE
        elif self.tok.isalpha():
            # identifier
            while self.src[self.cursor].isalnum() or self.src[self.cursor] == '_':
                self.cursor += 1
            self.val = self.src[self.pos:self.cursor]
            return TOK_ID
        elif self.tok == '\n':
            if self.cursor == len(self.src):
                return TOK_EOF
            self.line += 1
            self.line_pos = self.cursor
        elif not self.tok.isspace():
            raise KconfigParserError(self, 'Stray "%s"' % self.tok)

        return None

if __name__ == '__main__':
    fname = len(sys.argv) > 1 and sys.argv[1] or 'Kconfig.test'
    KconfigParser.parse(open(fname, 'r'))
