import java.io.DataInput;
import java.io.IOException;

class ChdescMoveFactory extends ModuleOpcodeFactory
{
	public ChdescMoveFactory(DataInput input)
	{
		super(input, KDB_CHDESC_MOVE);
		addParameter("chdesc", 4);
		addParameter("destination", 4);
		addParameter("target", 4);
		addParameter("offset", 2);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_MOVE"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescMove readChdescMove() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescMove();
	}
}

public class ChdescMove extends Opcode
{
	public ChdescMove(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescMoveFactory(input);
	}
}
