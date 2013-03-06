package org.servalproject.servald;

import java.lang.reflect.*;
import java.util.Arrays;
import java.util.List;
import java.util.LinkedList;
import org.servalproject.servald.ServalD;

class ServalDTests
{
	public static void main(String[] args)
	{
		try {
			Method m = ServalDTests.class.getMethod(args[0], String[].class);
			m.invoke(null, (Object) Arrays.copyOfRange(args, 1, args.length));
		}
		catch (Exception e) {
			e.printStackTrace();
			System.exit(1);
		}
		System.exit(0);
	}

	public static void repeat(String[] args)
	{
		int repeat = Integer.decode(args[0]);
		ServalD servald = new ServalD();
		for (int i = 0; i != repeat; ++i) {
			servald.command(Arrays.copyOfRange(args, 1, args.length));
			System.out.print(servald.status);
			for (byte[] a: servald.outv) {
				System.out.print(":");
				System.out.print(new String(a));
			}
			System.out.println("");
		}
		System.exit(0);
	}

	public static void nullArg(String[] args)
	{
		ServalD servald = new ServalD();
		for (int i = 0; i != args.length; ++i)
			if ("(null)".equals(args[i]))
				args[i] = null;
		servald.command(Arrays.copyOfRange(args, 0, args.length));
		System.out.print(servald.status);
		for (byte[] a: servald.outv) {
			System.out.print(":");
			System.out.print(new String(a));
		}
		System.out.println("");
		System.exit(0);
	}

}
