//import java.util.*;

import command.*;

public class JumpCommand implements Command
{
	public String getName()
	{
		return "jump";
	}
	
	public String getHelp()
	{
		return "Jump system state to a specified number of opcodes.";
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Debugger dbg = (Debugger) data;
		if(args.length == 0)
			System.out.println("Need an opcode to jump to.");
		else if(dbg != null)
		{
			try {
				int count = Integer.parseInt(args[0]);
				System.out.print("Replaying log... ");
				if(dbg.getApplied() > count)
					dbg.resetState();
				else
					count -= dbg.getApplied();
				if(dbg.replay(count))
					System.out.println(dbg.getApplied() + " opcodes OK!");
				else
					System.out.println(dbg.getApplied() + " opcodes OK, no change.");
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
