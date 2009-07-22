      subroutine rfile2(fname,buf,n,nr)

C  Write a wave file to disk.

      integer RMODE
      parameter(RMODE=0)
      integer*1 buf(n)
      integer open,read,close
      integer fd
      character fname*80
      data iz/0/                            !Silence g77 warning

      do i=80,1,-1
         if(fname(i:i).ne.' ') then
            iz=i
            go to 10
         endif
      enddo

 10   fname=fname(1:iz)//char(0)
      call cs_lock('rfile2')
      fd=open(fname,RMODE)                  !Open file for reading
      nr=read(fd,buf,n)
      i=close(fd)
      call cs_unlock

      return
      end
