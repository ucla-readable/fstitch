import java.io.DataInput;
import java.io.IOException;

class ChdescSetOwnerFactory extends ModuleOpcodeFactory
{
	public ChdescSetOwnerFactory(DataInput input)
	{
		super(input, KDB_CHDESC_SET_OWNER);
		addParameter("chdesc", 4);
		addParameter("owner", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_SET_OWNER"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescSetOwner readChdescSetOwner() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescSetOwner();
	}
}

public class ChdescSetOwner extends Opcode
{
	public ChdescSetOwner(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescSetOwnerFactory(input);
	}
}
