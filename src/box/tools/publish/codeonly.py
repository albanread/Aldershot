#!/usr/bin/env python3
"""codeonly.py BASE [FILES...]: check that each file differs from its
version at git revision BASE in comments and blank space only.

Rust, C, headers: // and /* */ comments removed (strings and character
literals respected), then all white space.  Python: the token stream
without COMMENT/NL/NEWLINE/INDENT/DEDENT tokens, and docstrings blanked.
Assembler (.s): text after ';' removed, then white space.  Exits 1 and
names the file if any code changed.  With no FILES, every changed file
under the current directory is checked."""
import io, subprocess, sys, tokenize, ast

def strip_c(s):
    out=[]; i=0; n=len(s)
    while i<n:
        c=s[i]
        if s.startswith('//',i):
            j=s.find('\n',i); i=n if j<0 else j; continue
        if s.startswith('/*',i):
            j=s.find('*/',i+2); i=n if j<0 else j+2; out.append(' '); continue
        if c=='"':
            j=i+1
            while j<n and s[j]!='"':
                j+=2 if s[j]=='\\' else 1
            out.append(s[i:j+1]); i=j+1; continue
        if c=="'":
            # C char literal, or a Rust lifetime / char literal
            m=s[i:i+4]
            if len(m)>=3 and (m[2]=="'" or (m[1]=='\\')):
                j=i+1
                while j<n and s[j]!="'":
                    j+=2 if s[j]=='\\' else 1
                out.append(s[i:j+1]); i=j+1; continue
        if c=='r' and (s.startswith('r"',i) or s.startswith('r#',i)) and (i==0 or not (s[i-1].isalnum() or s[i-1]=='_')):
            k=i+1; h=0
            while k<n and s[k]=='#': h+=1; k+=1
            if k<n and s[k]=='"':
                end='"'+'#'*h; j=s.find(end,k+1); j=n if j<0 else j+len(end)
                out.append(s[i:j]); i=j; continue
        out.append(c); i+=1
    return ''.join(ch for ch in ''.join(out) if not ch.isspace())

def strip_py(s):
    try:
        tree=ast.parse(s)
    except SyntaxError as e:
        return 'SYNTAX ERROR %s'%e
    for node in ast.walk(tree):
        if isinstance(node,(ast.Module,ast.FunctionDef,ast.AsyncFunctionDef,ast.ClassDef)) and node.body:
            b=node.body[0]
            if isinstance(b,ast.Expr) and isinstance(getattr(b,'value',None),ast.Constant) and isinstance(b.value.value,str):
                b.value.value=''
    return ast.dump(tree)

def strip_s(s):
    lines=[]
    for l in s.split('\n'):
        q=False; o=[]
        for ch in l:
            if ch=='"': q=not q
            if ch==';' and not q: break
            o.append(ch)
        lines.append(''.join(o))
    return ''.join(ch for ch in '\n'.join(lines) if not ch.isspace())

def norm(path,s):
    if path.endswith(('.rs','.c','.h')): return strip_c(s)
    if path.endswith('.py'): return strip_py(s)
    if path.endswith('.s'): return strip_s(s)
    return None

def main():
    base=sys.argv[1]; files=sys.argv[2:]
    if not files:
        files=subprocess.run(['git','diff','--name-only',base,'--','.'],capture_output=True,text=True,check=True).stdout.split()
        root=subprocess.run(['git','rev-parse','--show-toplevel'],capture_output=True,text=True).stdout.strip()
        import os
        files=[os.path.relpath(os.path.join(root,f)) for f in files]
    bad=0
    for f in files:
        new=open(f,encoding='latin-1').read()
        rel=subprocess.run(['git','ls-files','--full-name',f],capture_output=True,text=True).stdout.strip()
        old=subprocess.run(['git','show',f'{base}:{rel}'],capture_output=True,text=True,encoding='latin-1').stdout
        a,b=norm(f,old),norm(f,new)
        if a is None:
            print(f'{f}: not checked (not .rs .c .h .py .s)'); continue
        if a!=b:
            bad+=1
            i=next((k for k in range(min(len(a),len(b))) if a[k]!=b[k]),min(len(a),len(b)))
            print(f'{f}: CODE CHANGED near ...{a[max(0,i-60):i+60]!r}\n   now ...{b[max(0,i-60):i+60]!r}')
        else:
            print(f'{f}: comments only')
    sys.exit(1 if bad else 0)
main()
