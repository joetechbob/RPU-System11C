import sys,re
def strip(s):
    out=[];i=0;n=len(s)
    while i<n:
        c=s[i]
        if c=='"' or c=="'":
            q=c;j=i+1
            while j<n and s[j]!=q:
                j+=2 if s[j]=='\\' else 1
            out.append(s[i:j+1]);i=j+1
        elif s.startswith('//',i):
            j=s.find('\n',i); j=n if j<0 else j
            i=j
        elif s.startswith('/*',i):
            j=s.find('*/',i+2); i=j+2
            out.append(' ')
        else:
            out.append(c);i+=1
    t=''.join(out)
    # drop trailing whitespace and blank lines (comment removal leaves them)
    return '\n'.join(l.rstrip() for l in t.split('\n') if l.strip())
print(strip(open(sys.argv[1]).read()))
