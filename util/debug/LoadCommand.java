//import java.util.*;
import java.io.*;

import command.*;

public class LoadCommand implements Command
{
	public String getName()
	{
		return "load";
	}
	
	public String getHelp()
	{
		return "Load a new file to debug, with optional maximum number of opcodes.";
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Debugger dbg = (Debugger) data;
		if(args.length == 0)
			System.out.println("Need a file to load.");
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
					count = dbg.readOpcodes(size);
					System.out.println(count + " opcodes OK!");
				}
				else
					try {
						count = Integer.parseInt(args[1]);
						System.out.print("Reading debugging output... ");
						count = dbg.readOpcodes(count, size);
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
