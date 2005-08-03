import java.util.*;

import command.*;

public class ListCommand implements Command
{
	public static final String escseq = ((char) 27) + "[";
	public static final String bold = escseq + "1m";
	public static final String dim = escseq + "30m" + bold;
	public static final String normal = escseq + "0m";
	
	public String getName()
	{
		return "list";
	}
	
	public String getHelp()
	{
		return "List opcodes in a specified range, or all opcodes by default.";
	}
	
	private void listOpcode(int number, Opcode opcode)
	{
		if(opcode.hasEffect())
			System.out.println("#" + number + " " + opcode);
		else
			System.out.println(dim + "#" + number + " " + opcode + normal);
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Debugger dbg = (Debugger) data;
		if(dbg != null)
		{
			try {
				if(args.length == 0)
				{
					int i, max = dbg.getOpcodeCount();
					for(i = 0; i != max; i++)
						listOpcode(i + 1, dbg.getOpcode(i));
				}
				else if(args.length == 1)
				{
					int i, max = dbg.getOpcodeCount();
					i = Integer.parseInt(args[0]) - 1;
					if(0 <= i && i < max)
						listOpcode(i + 1, dbg.getOpcode(i));
					else
						System.out.println("No such opcode.");
				}
				else
				{
					int i, j, max = dbg.getOpcodeCount();
					i = Integer.parseInt(args[0]) - 1;
					j = Integer.parseInt(args[1]);
					/* note we allow the range to extend past the
					   end, so long as the beginning is valid */
					if(0 <= i && i < j && i < max)
						for(; i < j && i < max; i++)
							listOpcode(i + 1, dbg.getOpcode(i));
					else
						System.out.println("Invalid range.");
				}
			}
			catch(NumberFormatException e)
			{
				System.out.println("Invalid number.");
			}
		}
		else
			System.out.println("Need a file to debug.");
		return data;
	}
}
