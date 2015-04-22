me:
	@true

a:
	@true

sandwich:
	@if [ `id -u` != 0 ]; then echo "What? Make it yourself."; else echo "Ok."; fi

clean:
	rm -f *.o *~ thread_runner
