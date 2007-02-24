import java.io.*;

import command.*;

public class StreamCommand implements Command
{
	public String getName()
	{
		return "stream";
	}
	
	public String getHelp()
	{
		return "Stream a file to debug, with optional maximum number of opcodes.";
	}
	
	public static int readOpcodes(Debugger dbg, long size) throws BadInputException, IOException
	{
		long nextPercent = 1;
		long nextOffset = size * nextPercent / 100;
		boolean star = false;
		int opcodes = 0;
		try {
			for(;;)
			{
				try {
					dbg.replay(dbg.readOpcode());
				}
				catch(RuntimeException e)
				{
					System.out.println("interrupted! (" + opcodes + " opcodes OK)");
					throw e;
				}
				opcodes++;
				while(dbg.getInputOffset() >= nextOffset)
				{
					star = true;
					System.out.print("*");
					nextOffset = size * ++nextPercent / 100;
				}
			}
		}
		catch(EOFException e)
		{
			/* this is OK, we expect the end of the file sooner or later */
		}
		finally
		{
			if(star)
				System.out.print(" ");
		}
		return opcodes;
	}
	
	public static int readOpcodes(Debugger dbg, int count, long size) throws BadInputException, IOException
	{
		long nextPercent = 1;
		long nextOffset = size * nextPercent / 100;
		boolean star = false;
		int opcodes = 0;
		try {
			while(count-- > 0)
			{
				try {
					dbg.replay(dbg.readOpcode());
				}
				catch(RuntimeException e)
				{
					System.out.println("interrupted! (" + opcodes + " opcodes OK)");
					throw e;
				}
				opcodes++;
				while(dbg.getInputOffset() >= nextOffset)
				{
					star = true;
					System.out.print("*");
					nextOffset = size * ++nextPercent / 100;
				}
			}
		}
		catch(EOFException e)
		{
			/* this is OK, we expect the end of the file sooner or later */
		}
		finally
		{
			if(star)
				System.out.print(" ");
		}
		return opcodes;
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Debugger dbg = (Debugger) data;
		if(args.length == 0)
			System.out.println("Need a file to stream.");
		else if(dbg == null)
		{
			try {
				int count;
				File file = new File(args[0]);
				long size = file.length();
				InputStream stream = new BufferedInputStream(new FileInputStream(file), 1024 * 1024);
				DataInput input = new DataInputStream(stream);
				
				System.out.print("Reading debug signature... ");
				dbg = new Debugger(args[0], new CountingDataInput(input));
				System.out.println("OK!");
				
				if(args.length == 1)
				{
					System.out.print("Reading debugging output... ");
					count = readOpcodes(dbg, size);
					System.out.println(count + " opcodes OK!");
				}
				else
					try {
						count = Integer.parseInt(args[1]);
						System.out.print("Reading debugging output... ");
						count = readOpcodes(dbg, count, size);
						System.out.println(count + " opcodes OK!");
					}
					catch(NumberFormatException e)
					{
						System.out.println("Invalid number.");
						dbg = null;
					}
			}
			catch(UnsupportedStreamRevisionException e)
			{
				System.out.println(e);
				dbg = null;
			}
			catch(BadInputException e)
			{
				String message = "Bad input (" + e.getMessage() + " before byte " + e.offset + ") while reading " + args[0];
				if(dbg != null)
					message += "; " + dbg.getOpcodeCount() + " opcodes OK";
				System.out.println(message);
			}
			catch(IOException e)
			{
				System.out.println("I/O error while reading " + args[0]);
				dbg = null;
			}
		}
		else
			System.out.println(dbg.name + " is already loaded.");
		return dbg;
	}
}
