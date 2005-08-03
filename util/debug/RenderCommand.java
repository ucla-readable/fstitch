import java.util.*;
import java.io.*;

import command.*;

public class RenderCommand implements Command
{
	public String getName()
	{
		return "render";
	}
	
	public String getHelp()
	{
		return "Render system state to a GraphViz dot file, or standard output by default.";
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Debugger dbg = (Debugger) data;
		if(dbg != null)
		{
			try {
				Writer writer;
				if(args.length != 0)
					writer = new FileWriter(new File(args[0]));
				else
					writer = new OutputStreamWriter(System.out);
				dbg.getState().render(writer, true);
			}
			catch(IOException e)
			{
				System.out.println("I/O error while writing to output.");
			}
		}
		else
			System.out.println("Need a file to debug.");
		return data;
	}
}
